// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

/*
 * irblasterd - org.webosports.service.ir
 *
 * Puts the infrared transmitter on the bus. The API is the one Android's
 * ConsumerIrManager settled on - a carrier frequency and a list of alternating
 * mark/space durations in microseconds - because that is the common ground of
 * every transmitter we know of: protocol encoding (NEC, RC5, ...) stays in the
 * caller, and the service only has to know how to get timings out of the LED.
 *
 * Two backends, picked at runtime by what the device has:
 *
 *  - sec_ir: Samsung's iCE40 FPGA (drivers/ice4_fpga) on the Exynos 5420
 *    tablets and a few Galaxy phones. Takes "carrier,us,us,..." as text in
 *    /sys/class/sec/sec_ir/ir_send and reports the FPGA's acknowledgement in
 *    ir_send_result.
 *  - lirc: the mainline /dev/lirc0 interface (gpio-ir-tx, ir-spi, pwm-ir-tx,
 *    USB transceivers) - carrier set by ioctl, then the durations written as
 *    an array of unsigned ints.
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib-unix.h>
#include <linux/lirc.h>
#include <luna-service2/lunaservice.h>
#include <pbnjson.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define IR_SERVICE "org.webosports.service.ir"

/* Overridable so the tests can point the backends at files they control */
#ifndef SEC_IR_SEND
#define SEC_IR_SEND   "/sys/class/sec/sec_ir/ir_send"
#endif
#ifndef SEC_IR_RESULT
#define SEC_IR_RESULT "/sys/class/sec/sec_ir/ir_send_result"
#endif
#ifndef LIRC_DEVICE
#define LIRC_DEVICE   "/dev/lirc0"
#endif

/*
 * OPEN_FLAGS: every node this service opens is a fixed path under /sys or /dev
 * that only root can create, and irblasterd is the root service that uses
 * them, so there is no attacker-controlled path to race. O_NOFOLLOW is there
 * anyway: the last component of each is a real attribute or device node, never
 * a symlink, so refusing one costs nothing and closes the obvious trick should
 * a node ever be replaced. (The /sys/class/... directories above it are
 * symlinks by design; only the final component is checked.)
 */

/*
 * Limits come from the sec_ir driver, which is the stricter of the two and
 * trusts its caller completely:
 *
 *  - It copies each duration into a fixed 2048-byte buffer, two bytes apiece
 *    after a five-byte header, without checking the index. Anything over 1021
 *    durations writes past the end of the buffer in kernel memory.
 *  - It turns each duration into carrier cycles as us / (1000000 / carrier),
 *    both divisions truncating, and stores the result in 16 bits. A zero ends
 *    the pattern early and anything over 65535 wraps, so every duration has to
 *    come out between 1 and 65535 by that same arithmetic - checking it with
 *    the exact carrier instead lets through durations the driver then wraps.
 *  - It sums the cycles in an int, sleeps 1000 * sum / carrier milliseconds in
 *    the writer's context and overflows past about 2^31 / 1000 cycles. A burst
 *    is therefore capped at MAX_BURST_US, far above any real remote (the
 *    longest sane code in Flipper-IRDB, an air conditioner's, is under two
 *    seconds) and far below where the sum overflows.
 *  - The whole pattern goes through one sysfs write, which the kernel caps at
 *    a page.
 *
 * The lirc backend gets the same limits: they are generous for any remote,
 * and one set of rules is easier for callers than two.
 */
#define MAX_DURATIONS      1000
#define MAX_CYCLES         65535
#define MAX_BURST_US       5000000
#define MIN_CARRIER_HZ     15000
#define MAX_CARRIER_HZ     500000
#define SYSFS_WRITE_LIMIT  4096

/*
 * Requests waiting behind the one on the LED. A held key sends one at a time
 * and waits for the reply, so this only ever fills up when something floods
 * the service - and then each queued pattern is 4 KB we would hold forever.
 */
#define MAX_QUEUED         8

typedef enum
{
	BACKEND_NONE,
	BACKEND_SEC_IR,
	BACKEND_LIRC,
} Backend;

typedef struct
{
	LSMessage *message;
	guint frequency;
	guint *pattern;
	guint count;
	char *error;
	/* The FPGA confirmed the burst; see transmit_sec_ir for why it may not */
	bool acknowledged;
} TransmitJob;

static LSHandle *service_handle = NULL;
static GMainLoop *main_loop = NULL;
static GThreadPool *transmit_pool = NULL;
static int exit_status = 0;
/* Set by main() on the way out; queued jobs are then answered, not sent */
static gint stopping = 0;
/* Written once in main() before the worker exists, read-only after that */
static Backend backend = BACKEND_NONE;

static const char *backend_name(Backend b)
{
	switch (b)
	{
	case BACKEND_SEC_IR:
		return "sec_ir";

	case BACKEND_LIRC:
		return "lirc";

	case BACKEND_NONE:
	default:
		return "none";
	}
}

/*
 * Probed once at start-up by opening the nodes the way they will be used, not
 * by asking access(): a check that is separate from the use only answers for
 * the moment it ran. Neither node comes and goes at runtime - sec_ir is a
 * board device and lirc0 a platform one - and every later open is checked
 * anyway, so a node that vanishes turns into an error reply, not a crash.
 *
 * A device without either is a normal state: getStatus answers
 * "available: false" rather than the service refusing to start.
 */
static Backend probe_backend(void)
{
	unsigned int features = 0;
	bool can_send;
	int fd;

	fd = open(SEC_IR_SEND, O_WRONLY | O_CLOEXEC | O_NOFOLLOW); /* Flawfinder: ignore - fixed root-owned node, see OPEN_FLAGS */

	if (fd >= 0)
	{
		close(fd);
		return BACKEND_SEC_IR;
	}

	fd = open(LIRC_DEVICE, O_RDWR | O_CLOEXEC | O_NOFOLLOW); /* Flawfinder: ignore - fixed root-owned node, see OPEN_FLAGS */

	if (fd < 0)
	{
		return BACKEND_NONE;
	}

	can_send = ioctl(fd, LIRC_GET_FEATURES, &features) == 0 &&
	           (features & LIRC_CAN_SEND_PULSE) != 0;
	close(fd);

	return can_send ? BACKEND_LIRC : BACKEND_NONE;
}

static char *transmit_sec_ir(guint frequency, const guint *pattern, guint count, bool *acknowledged)
{
	GString *text = g_string_sized_new(SYSFS_WRITE_LIMIT);
	char result[8] = { 0 };
	ssize_t written;
	ssize_t got;
	gsize length;
	guint i;
	int fd;

	g_string_append_printf(text, "%u", frequency);

	for (i = 0; i < count; i++)
	{
		g_string_append_printf(text, ",%u", pattern[i]);
	}

	length = text->len;

	if (length >= SYSFS_WRITE_LIMIT)
	{
		g_string_free(text, TRUE);
		return g_strdup("pattern too long for the transmitter");
	}

	fd = open(SEC_IR_SEND, O_WRONLY | O_CLOEXEC | O_NOFOLLOW); /* Flawfinder: ignore - fixed root-owned node, see OPEN_FLAGS */

	if (fd < 0)
	{
		int saved = errno;
		g_string_free(text, TRUE);
		return g_strdup_printf("cannot open %s: %s", SEC_IR_SEND, g_strerror(saved));
	}

	/*
	 * The driver blocks in this write for as long as the pattern takes to
	 * send, which is why transmissions run on the worker thread. It returns 1
	 * rather than the length when the FPGA has no firmware.
	 */
	written = write(fd, text->str, length);
	close(fd);
	g_string_free(text, TRUE);

	if (written < 0 || (gsize)written != length)
	{
		return g_strdup("the transmitter is not ready (FPGA firmware not loaded)");
	}

	/*
	 * "1" when the FPGA confirmed the burst. The driver keeps one
	 * acknowledgement for everybody, so this reads ours only because
	 * irblasterd is the only writer (the node is root's, and this is the root
	 * service that owns it) and sends one burst at a time.
	 *
	 * It is reported, not acted on. The driver forms it from the FPGA's IRQ
	 * line sampled a fixed 10 ms after the write, and on the SM-T520 some
	 * short bursts (a few durations, a few milliseconds) come back
	 * unconfirmed although the driver then logs them as sent - while every
	 * real remote code tried, NEC, RC5 and the rest, is confirmed. Failing the
	 * call on it would show users errors for bursts that did go out.
	 */
	fd = open(SEC_IR_RESULT, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); /* Flawfinder: ignore - fixed root-owned node, see OPEN_FLAGS */

	if (fd < 0)
	{
		g_warning("cannot read %s: %s", SEC_IR_RESULT, g_strerror(errno));
		*acknowledged = false;
		return NULL;
	}

	/* Flawfinder: ignore - bounded to the zeroed buffer, NUL kept */
	got = read(fd, result, sizeof(result) - 1);
	close(fd);

	*acknowledged = got > 0 && result[0] == '1';

	if (!*acknowledged)
	{
		g_warning("the transmitter did not confirm a %u-duration burst", count);
	}

	return NULL;
}

static char *transmit_lirc(guint frequency, const guint *pattern, guint count)
{
	unsigned int carrier = frequency;
	ssize_t written;
	size_t length;
	int fd;

	fd = open(LIRC_DEVICE, O_RDWR | O_CLOEXEC | O_NOFOLLOW); /* Flawfinder: ignore - fixed root-owned node, see OPEN_FLAGS */

	if (fd < 0)
	{
		return g_strdup_printf("cannot open %s: %s", LIRC_DEVICE, g_strerror(errno));
	}

	/* Not every LIRC transmitter can change its carrier; that is not fatal */
	if (ioctl(fd, LIRC_SET_SEND_CARRIER, &carrier) < 0)
	{
		g_warning("%s: cannot set a %u Hz carrier: %s", LIRC_DEVICE, frequency,
		          g_strerror(errno));
	}

	/* LIRC wants an odd count - it must end on a pulse. count is at least 1. */
	length = (size_t)(count % 2 != 0 ? count : count - 1) * sizeof(*pattern);
	written = write(fd, pattern, length);

	if (written < 0 || (size_t)written != length)
	{
		int saved = errno;
		close(fd);
		return g_strdup_printf("%s rejected the pattern: %s", LIRC_DEVICE,
		                       written < 0 ? g_strerror(saved) : "short write");
	}

	close(fd);
	return NULL;
}

static void reply_json(LSHandle *sh, LSMessage *message, jvalue_ref reply)
{
	LSError lserror;

	LSErrorInit(&lserror);

	if (!LSMessageReply(sh, message, jvalue_tostring_simple(reply), &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}
}

static bool reply_error(LSHandle *sh, LSMessage *message, const char *text)
{
	jvalue_ref reply = jobject_create();

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(false));
	jobject_put(reply, J_CSTR_TO_JVAL("errorText"), jstring_create(text));
	reply_json(sh, message, reply);
	j_release(&reply);
	return true;
}

static void job_free(gpointer data)
{
	TransmitJob *job = data;

	LSMessageUnref(job->message);
	g_free(job->pattern);
	g_free(job->error);
	g_free(job);
}

/* Back on the main loop: luna-service2 replies belong to the thread that owns the handle */
static gboolean transmit_done(gpointer data)
{
	TransmitJob *job = data;
	jvalue_ref reply = jobject_create();

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(job->error == NULL));

	if (job->error)
	{
		jobject_put(reply, J_CSTR_TO_JVAL("errorText"), jstring_create(job->error));
	}
	else
	{
		jobject_put(reply, J_CSTR_TO_JVAL("acknowledged"), jboolean_create(job->acknowledged));
	}

	reply_json(service_handle, job->message, reply);
	j_release(&reply);
	job_free(job);
	return G_SOURCE_REMOVE;
}

/*
 * One worker, so transmissions go out in the order they were asked for rather
 * than interleaving on the LED - two at once would garble both. The job
 * belongs to the worker until it hands it back to the main loop, and nothing
 * else touches it in between.
 */
static void transmit_worker(gpointer data, gpointer user_data)
{
	TransmitJob *job = data;

	if (g_atomic_int_get(&stopping))
	{
		job->error = g_strdup("the service is shutting down");
	}
	else if (backend == BACKEND_SEC_IR)
	{
		job->error = transmit_sec_ir(job->frequency, job->pattern, job->count, &job->acknowledged);
	}
	else
	{
		/* lirc's write only returns once the pulses are out: that is its confirmation */
		job->error = transmit_lirc(job->frequency, job->pattern, job->count);
		job->acknowledged = job->error == NULL;
	}

	g_main_context_invoke(NULL, transmit_done, job);
}

static jvalue_ref build_status(void)
{
	jvalue_ref reply = jobject_create();

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("available"), jboolean_create(backend != BACKEND_NONE));
	jobject_put(reply, J_CSTR_TO_JVAL("backend"), jstring_create(backend_name(backend)));
	jobject_put(reply, J_CSTR_TO_JVAL("maxDurations"), jnumber_create_i32(MAX_DURATIONS));
	return reply;
}

static bool cb_get_status(LSHandle *sh, LSMessage *message, void *ctx)
{
	LSError lserror;
	jvalue_ref status;
	bool subscribed = false;

	LSErrorInit(&lserror);

	if (LSMessageIsSubscription(message) &&
	        !LSSubscriptionProcess(sh, message, &subscribed, &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}

	status = build_status();
	reply_json(sh, message, status);
	j_release(&status);
	return true;
}

/*
 * Checks a pattern against the limits above and copies it out of the request.
 * Returns NULL and sets *error on the first thing wrong with it. Durations too
 * short to be a single carrier cycle are raised to one - a zero would end the
 * driver's pattern - rather than refused.
 */
static guint *parse_pattern(jvalue_ref pattern, guint frequency, guint *count, const char **error)
{
	/* The driver's own divisor; frequency is at most MAX_CARRIER_HZ, so never 0 */
	const guint cycle_us = 1000000 / frequency;
	ssize_t size = jarray_size(pattern);
	guint64 total_us = 0;
	guint *durations;
	ssize_t i;

	if (size < 1)
	{
		*error = "need \"pattern\": array of durations in microseconds";
		return NULL;
	}

	if (size > MAX_DURATIONS)
	{
		*error = "pattern has more durations than the transmitter takes";
		return NULL;
	}

	durations = g_new0(guint, (gsize)size);

	for (i = 0; i < size; i++)
	{
		jvalue_ref item = jarray_get(pattern, i);
		int32_t duration = 0;
		guint value;

		if (!jis_number(item) || jnumber_get_i32(item, &duration) != CONV_OK || duration <= 0)
		{
			g_free(durations);
			*error = "durations must be positive whole numbers of microseconds";
			return NULL;
		}

		value = MAX((guint)duration, cycle_us);

		if (value / cycle_us > MAX_CYCLES)
		{
			g_free(durations);
			*error = "a duration is longer than the transmitter can count";
			return NULL;
		}

		total_us += value;

		if (total_us > MAX_BURST_US)
		{
			g_free(durations);
			*error = "pattern is longer than a burst may last";
			return NULL;
		}

		durations[i] = value;
	}

	*count = (guint)size;
	return durations;
}

/*
 * transmit takes {"frequency": Hz, "pattern": [us, us, ...]}, the pattern
 * starting with a mark. The reply comes once the burst has gone out, so a
 * caller that wants to pace repeats can simply wait for it.
 */
static bool cb_transmit(LSHandle *sh, LSMessage *message, void *ctx)
{
	JSchemaInfo schema;
	jvalue_ref parsed, value, pattern;
	const char *error = NULL;
	int32_t frequency = 0;
	TransmitJob *job;
	guint *durations;
	guint count = 0;

	if (backend == BACKEND_NONE)
	{
		return reply_error(sh, message, "no infrared transmitter on this device");
	}

	/* NULL once main() has stopped the worker and is draining the last replies */
	if (transmit_pool == NULL)
	{
		return reply_error(sh, message, "the service is shutting down");
	}

	if (g_thread_pool_unprocessed(transmit_pool) >= MAX_QUEUED)
	{
		return reply_error(sh, message, "the transmitter is busy");
	}

	jschema_info_init(&schema, jschema_all(), NULL, NULL);
	parsed = jdom_parse(j_cstr_to_buffer(LSMessageGetPayload(message)),
	                    DOMOPT_NOOPT, &schema);

	if (jis_null(parsed))
	{
		j_release(&parsed);
		return reply_error(sh, message, "malformed json");
	}

	if (!jobject_get_exists(parsed, J_CSTR_TO_BUF("frequency"), &value) ||
	        !jis_number(value) || jnumber_get_i32(value, &frequency) != CONV_OK ||
	        frequency < MIN_CARRIER_HZ || frequency > MAX_CARRIER_HZ)
	{
		j_release(&parsed);
		return reply_error(sh, message, "need \"frequency\": carrier in Hz, 15000-500000");
	}

	if (!jobject_get_exists(parsed, J_CSTR_TO_BUF("pattern"), &pattern) || !jis_array(pattern))
	{
		j_release(&parsed);
		return reply_error(sh, message, "need \"pattern\": array of durations in microseconds");
	}

	durations = parse_pattern(pattern, (guint)frequency, &count, &error);
	/* pattern and its items are borrowed from parsed: done with all of them */
	j_release(&parsed);

	if (durations == NULL)
	{
		return reply_error(sh, message, error);
	}

	job = g_new0(TransmitJob, 1);
	job->frequency = (guint)frequency;
	job->pattern = durations;
	job->count = count;
	job->message = message;
	LSMessageRef(message);

	/* Cannot fail: the pool is exclusive and was created in main() */
	g_thread_pool_push(transmit_pool, job, NULL);
	return true;
}

static LSMethod methods[] =
{
	{ "getStatus", cb_get_status, LUNA_METHOD_FLAGS_NONE },
	{ "transmit",  cb_transmit,   LUNA_METHOD_FLAGS_NONE },
	{ NULL, NULL, LUNA_METHOD_FLAGS_NONE },
};

/*
 * The hub went away. The registration died with it and nothing re-makes it,
 * so exit non-zero and let systemd start a fresh process - the same reasoning
 * as torchd.
 */
static void hub_disconnected(LSHandle *sh, void *ctx)
{
	g_warning("lost the luna-service2 hub; exiting so systemd restarts us with a "
	          "fresh registration");
	exit_status = 1;
	g_main_loop_quit(main_loop);
}

/* systemd stops us with SIGTERM: let the burst on the LED finish first */
static gboolean on_terminate(gpointer user_data)
{
	g_main_loop_quit(main_loop);
	return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
	LSError lserror;

	LSErrorInit(&lserror);
	main_loop = g_main_loop_new(NULL, FALSE);

	backend = probe_backend();
	g_message("infrared backend: %s", backend_name(backend));

	/* Exclusive, so the one thread exists from here on and push cannot fail */
	transmit_pool = g_thread_pool_new_full(transmit_worker, NULL, job_free, 1, TRUE, NULL);

	g_unix_signal_add(SIGTERM, on_terminate, NULL);
	g_unix_signal_add(SIGINT, on_terminate, NULL);

	if (!LSRegister(IR_SERVICE, &service_handle, &lserror) ||
	        !LSRegisterCategory(service_handle, "/", methods, NULL, NULL, &lserror) ||
	        !LSGmainAttach(service_handle, main_loop, &lserror) ||
	        !LSSetDisconnectHandler(service_handle, hub_disconnected, NULL, &lserror))
	{
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
		exit_status = 1;
	}
	else
	{
		g_main_loop_run(main_loop);
	}

	/*
	 * Lets the burst already on the LED finish and answers every job queued
	 * behind it with "shutting down", without sending it. Not with
	 * g_thread_pool_free's own immediate mode: glib drops a task a worker has
	 * already taken off the queue when it then sees the pool stopping, without
	 * running it and without item_free_func - one job and its message leaked
	 * per shutdown, now and then. This way every job goes through
	 * transmit_done and nowhere else. A burst that finished while
	 * the loop was still running has its reply waiting as an idle source the
	 * loop quit before dispatching, and one that finishes now has it run right
	 * in the worker (g_main_context_invoke on a context nobody owns) - safe,
	 * because this thread is parked in here meanwhile. Draining the context
	 * afterwards sends the first kind and frees its job; anything new that
	 * arrives meanwhile is told the service is going away.
	 */
	g_atomic_int_set(&stopping, 1);
	g_thread_pool_free(transmit_pool, FALSE, TRUE);
	transmit_pool = NULL;

	while (g_main_context_iteration(NULL, FALSE))
	{
	}

	if (service_handle)
	{
		if (!LSUnregister(service_handle, &lserror))
		{
			LSErrorFree(&lserror);
		}
	}

	g_main_loop_unref(main_loop);
	return exit_status;
}
