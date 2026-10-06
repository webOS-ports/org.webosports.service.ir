/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drives irblasterd's real main.c, unchanged, against the stand-in bus and
 * JSON library, with its device nodes pointed at files in a work directory
 * and write()/ioctl() routed through hooks that can fail on demand.
 *
 * Usage: irblasterd-test <sec_ir|lirc|none|sigterm|late-reply>
 *
 * Built once per sanitizer by run.sh; any sanitizer report, assertion or
 * failed check exits non-zero.
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <linux/lirc.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "shim.h"

/*
 * main.c's own main() becomes irblasterd_main(), and its write() and ioctl()
 * calls go through hooks that can fail on demand. The system headers are
 * already in, so only main.c's calls are renamed, not glibc's prototypes.
 */
ssize_t harness_write(int fd, const void *buf, size_t count);
int harness_ioctl(int fd, unsigned long request, ...);

/*
 * glib's thread pool and g_main_context_invoke hand data between threads
 * through queues locked with raw futexes, which ThreadSanitizer cannot see
 * into - so every job looks raced between the thread that built it and the
 * one that uses it. These wrappers tell it about the hand-over glib really
 * performs (release where glib takes the pointer, acquire where it gives it
 * back) and nothing more; outside a TSan build they are plain pass-throughs.
 */
#if defined(__SANITIZE_THREAD__)
#define HARNESS_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define HARNESS_TSAN 1
#endif
#endif
#ifdef HARNESS_TSAN
#include <sanitizer/tsan_interface.h>
#define HANDOFF_RELEASE(p) __tsan_release(p)
#define HANDOFF_ACQUIRE(p) __tsan_acquire(p)
#else
#define HANDOFF_RELEASE(p) ((void)(p))
#define HANDOFF_ACQUIRE(p) ((void)(p))
#endif

static GFunc real_pool_func;

static void harness_pool_trampoline(gpointer data, gpointer user_data)
{
	HANDOFF_ACQUIRE(data);
	real_pool_func(data, user_data);
}

static GThreadPool *harness_pool_new_full(GFunc func, gpointer user_data, GDestroyNotify item_free,
                                          gint max_threads, gboolean exclusive, GError **error)
{
	real_pool_func = func;
	return g_thread_pool_new_full(harness_pool_trampoline, user_data, item_free, max_threads, exclusive, error);
}

static gboolean harness_pool_push(GThreadPool *pool, gpointer data, GError **error)
{
	HANDOFF_RELEASE(data);
	return g_thread_pool_push(pool, data, error);
}

typedef struct { GSourceFunc func; gpointer data; } InvokeCall;

static gboolean harness_invoke_trampoline(gpointer user_data)
{
	InvokeCall *call = user_data;
	gboolean result;

	HANDOFF_ACQUIRE(call);
	HANDOFF_ACQUIRE(call->data);
	result = call->func(call->data);
	g_free(call);
	return result;
}

/* Lets a test wait until the worker has handed a reply back */
static pthread_mutex_t invoke_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t invoke_cond = PTHREAD_COND_INITIALIZER;
static int invokes = 0;

static void harness_context_invoke(GMainContext *context, GSourceFunc func, gpointer data)
{
	InvokeCall *call = g_new(InvokeCall, 1);

	call->func = func;
	call->data = data;
	HANDOFF_RELEASE(data);
	HANDOFF_RELEASE(call);
	g_main_context_invoke(context, harness_invoke_trampoline, call);

	pthread_mutex_lock(&invoke_lock);
	invokes++;
	pthread_cond_broadcast(&invoke_cond);
	pthread_mutex_unlock(&invoke_lock);
}

#define main irblasterd_main
#define write harness_write
#define ioctl harness_ioctl
#define g_thread_pool_new_full harness_pool_new_full
#define g_thread_pool_push harness_pool_push
#define g_main_context_invoke harness_context_invoke
/* run.sh can point this at another copy of main.c (IR_SRC), e.g. a mutant */
#ifndef IR_MAIN_C
#define IR_MAIN_C "../../src/main.c"
#endif
#include IR_MAIN_C
#undef main
#undef write
#undef ioctl
#undef g_thread_pool_new_full
#undef g_thread_pool_push
#undef g_main_context_invoke

typedef enum { FAULT_NONE, FAULT_EIO, FAULT_ONE, FAULT_SHORT } Fault;

static gint fault = FAULT_NONE;
static gint slow_write_ms = 0;
static pthread_mutex_t capture_lock = PTHREAD_MUTEX_INITIALIZER;
static GByteArray *captured = NULL;
static int failures = 0;
static const char *mode = "sec_ir";
static gint outstanding = 0;
static void (*next_phase)(void) = NULL;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; g_printerr("FAIL %s:%d: ", __FILE__, __LINE__); g_printerr(__VA_ARGS__); g_printerr("\n"); } } while (0)

/* While closed, the worker is held inside write(), with its burst "on the LED" */
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool gate_closed = false;
static bool gate_holding = false;

static void gate_set(bool closed)
{
	pthread_mutex_lock(&gate_lock);
	gate_closed = closed;
	pthread_cond_broadcast(&gate_cond);
	pthread_mutex_unlock(&gate_lock);
}

/* Until the worker is parked at the gate */
static void gate_wait_held(void)
{
	pthread_mutex_lock(&gate_lock);
	while (!gate_holding)
		pthread_cond_wait(&gate_cond, &gate_lock);
	pthread_mutex_unlock(&gate_lock);
}

ssize_t harness_write(int fd, const void *buf, size_t count)
{
	pthread_mutex_lock(&gate_lock);
	while (gate_closed)
	{
		gate_holding = true;
		pthread_cond_broadcast(&gate_cond);
		pthread_cond_wait(&gate_cond, &gate_lock);
	}
	gate_holding = false;
	pthread_mutex_unlock(&gate_lock);

	if (g_atomic_int_get(&slow_write_ms))
		g_usleep((gulong)g_atomic_int_get(&slow_write_ms) * 1000);

	pthread_mutex_lock(&capture_lock);
	g_byte_array_set_size(captured, 0);
	g_byte_array_append(captured, buf, (guint)count);
	pthread_mutex_unlock(&capture_lock);

	switch ((Fault)g_atomic_int_get(&fault))
	{
	case FAULT_EIO:   errno = EIO; return -1;
	case FAULT_ONE:   return 1;
	case FAULT_SHORT: return (ssize_t)(count / 2);
	case FAULT_NONE:
	default:          return (ssize_t)count;
	}
}

/* What the lirc mode's transmitter was last asked for */
static gint lirc_carrier = 0, lirc_duty = 0;

int harness_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	unsigned int *arg;

	va_start(ap, request);
	arg = va_arg(ap, unsigned int *);
	va_end(ap);

	if (request == LIRC_GET_FEATURES)
	{
		*arg = LIRC_CAN_SEND_PULSE | LIRC_CAN_SET_SEND_CARRIER | LIRC_CAN_SET_SEND_DUTY_CYCLE;
		return 0;
	}

	if (request == LIRC_SET_SEND_CARRIER)
	{
		g_atomic_int_set(&lirc_carrier, (gint)*arg);
		return 0;
	}

	if (request == LIRC_SET_SEND_DUTY_CYCLE)
	{
		g_atomic_int_set(&lirc_duty, (gint)*arg);
		return 0;
	}

	errno = ENOTTY;
	return -1;
}

static char *captured_text(void)
{
	char *s;

	pthread_mutex_lock(&capture_lock);
	s = g_strndup((const char *)captured->data, captured->len);
	pthread_mutex_unlock(&capture_lock);
	return s;
}

static void write_result(const char *value)
{
	g_assert(g_file_set_contents(SEC_IR_RESULT, value, -1, NULL));
}

/* ------------------------------------------------------------ expectations */

typedef struct
{
	const char *name;
	bool ok;            /* expected returnValue */
	const char *error;  /* substring the errorText must contain, or NULL */
	const char *sent;   /* exact sysfs text expected, or NULL */
} Expect;

static void phase_done_check(void)
{
	if (g_atomic_int_dec_and_test(&outstanding) && next_phase)
	{
		void (*f)(void) = next_phase;
		next_phase = NULL;
		f();
	}
}

static void on_expect(const char *request, const char *reply, gpointer user_data)
{
	Expect *e = user_data;
	bool ok = strstr(reply, "\"returnValue\":true") != NULL;

	CHECK(ok == e->ok, "%s: expected returnValue %d, got %s", e->name, e->ok, reply);

	if (e->error)
		CHECK(strstr(reply, e->error) != NULL, "%s: expected error '%s', got %s", e->name, e->error, reply);

	if (e->sent && ok)
	{
		char *text = captured_text();
		CHECK(strcmp(text, e->sent) == 0, "%s: sent '%.80s' expected '%.80s'", e->name, text, e->sent);
		g_free(text);
	}

	phase_done_check();
}

static void expect(Expect *e, const char *method, const char *payload)
{
	g_atomic_int_inc(&outstanding);
	shim_call(method, payload, on_expect, e);
}

static char *repeat_pattern(const char *value, int n)
{
	GString *s = g_string_new("[");

	for (int i = 0; i < n; i++)
		g_string_append_printf(s, "%s%s", i ? "," : "", value);

	g_string_append_c(s, ']');
	return g_string_free(s, FALSE);
}

static char *transmit_payload(int frequency, const char *pattern)
{
	return g_strdup_printf("{\"frequency\":%d,\"pattern\":%s}", frequency, pattern);
}

/* ------------------------------------------------------------------ phases */

static void phase_shutdown(void);
static void phase_fuzz(void);
static void phase_flood(void);
static void phase_faults(void);
static void phase_ack(void);

static GPtrArray *keep;   /* payload strings and Expects live until exit */

static Expect *mk(const char *name, bool ok, const char *error, const char *sent)
{
	Expect *e = g_new0(Expect, 1);
	e->name = name; e->ok = ok; e->error = error; e->sent = sent;
	g_ptr_array_add(keep, e);
	return e;
}

static const char *own(char *s)
{
	g_ptr_array_add(keep, s);
	return s;
}

typedef struct { Expect *e; const char *method; const char *payload; } Case;
static GArray *cases;
static guint case_index = 0;

static void add_case(Expect *e, const char *method, const char *payload)
{
	Case c = { e, method, payload };
	g_array_append_val(cases, c);
}

/*
 * One case at a time, each sent once the last has been answered: the check of
 * what reached the sysfs node has to see that case's write and no other.
 */
static void run_next_case(void);

static void on_case(const char *request, const char *reply, gpointer user_data)
{
	g_atomic_int_inc(&outstanding);
	on_expect(request, reply, user_data);
	run_next_case();
}

static void run_next_case(void)
{
	Case *c;

	if (case_index >= cases->len)
	{
		g_array_free(cases, TRUE);
		phase_ack();
		return;
	}

	c = &g_array_index(cases, Case, case_index++);
	shim_call(c->method, c->payload, on_case, c->e);
}

static void phase_validation(void)
{
	/* NEC LG Mute: the bytes the FPGA must get for the first frame */
	static const char lg_mute[] =
		"[9000,4500,560,560,560,560,560,1690,560,560,560,560,560,560,560,560,560,560,"
		"560,1690,560,1690,560,560,560,1690,560,1690,560,1690,560,1690,560,1690,"
		"560,1690,560,560,560,560,560,1690,560,560,560,560,560,560,560,560,560,560,"
		"560,1690,560,1690,560,560,560,1690,560,1690,560,1690,560,1690,560]";

	cases = g_array_new(FALSE, FALSE, sizeof(Case));

	add_case(mk("status", true, "\"backend\":\"sec_ir\"", NULL), "getStatus", "{}");
	add_case(mk("status-sub", true, NULL, NULL), "getStatus", "{\"subscribe\":true}");

	{
		char *sent = g_strdup_printf("38000,%s", lg_mute + 1);
		sent[strlen(sent) - 1] = '\0';
		add_case(mk("lg-mute", true, "\"acknowledged\":true", own(sent)), "transmit", own(transmit_payload(38000, lg_mute)));
	}

	/* Shorter than a carrier cycle: raised to the driver's divisor, 1000000/38000 = 26 */
	add_case(mk("raise-short", true, NULL, "38000,26,560"), "transmit", "{\"frequency\":38000,\"pattern\":[1,560]}");

	add_case(mk("not-json", false, "malformed json", NULL), "transmit", "garbage");
	add_case(mk("empty-payload", false, "malformed json", NULL), "transmit", "");
	add_case(mk("trailing", false, "malformed json", NULL), "transmit", "{\"frequency\":38000} x");
	add_case(mk("no-freq", false, "frequency", NULL), "transmit", "{}");
	add_case(mk("array-root", false, "frequency", NULL), "transmit", "[1,2]");
	add_case(mk("freq-low", false, "frequency", NULL), "transmit", "{\"frequency\":14999,\"pattern\":[560]}");
	add_case(mk("freq-high", false, "frequency", NULL), "transmit", "{\"frequency\":500001,\"pattern\":[560]}");
	add_case(mk("freq-float", false, "frequency", NULL), "transmit", "{\"frequency\":38000.5,\"pattern\":[560]}");
	add_case(mk("freq-string", false, "frequency", NULL), "transmit", "{\"frequency\":\"38000\",\"pattern\":[560]}");
	add_case(mk("freq-huge", false, "frequency", NULL), "transmit", "{\"frequency\":99999999999,\"pattern\":[560]}");
	add_case(mk("no-pattern", false, "pattern", NULL), "transmit", "{\"frequency\":38000}");
	add_case(mk("pattern-string", false, "pattern", NULL), "transmit", "{\"frequency\":38000,\"pattern\":\"560\"}");
	add_case(mk("pattern-object", false, "pattern", NULL), "transmit", "{\"frequency\":38000,\"pattern\":{}}");
	add_case(mk("pattern-empty", false, "pattern", NULL), "transmit", "{\"frequency\":38000,\"pattern\":[]}");
	add_case(mk("zero", false, "positive", NULL), "transmit", "{\"frequency\":38000,\"pattern\":[560,0,560]}");
	add_case(mk("negative", false, "positive", NULL), "transmit", "{\"frequency\":38000,\"pattern\":[-5]}");
	add_case(mk("float-dur", false, "positive", NULL), "transmit", "{\"frequency\":38000,\"pattern\":[560.5]}");
	add_case(mk("string-dur", false, "positive", NULL), "transmit", "{\"frequency\":38000,\"pattern\":[\"560\"]}");
	add_case(mk("null-dur", false, "positive", NULL), "transmit", "{\"frequency\":38000,\"pattern\":[null]}");
	add_case(mk("int-overflow", false, "positive", NULL), "transmit", "{\"frequency\":38000,\"pattern\":[2147483648]}");
	add_case(mk("1001", false, "more durations", NULL), "transmit", own(transmit_payload(38000, own(repeat_pattern("100", 1001)))));
	add_case(mk("1000", true, NULL, NULL), "transmit", own(transmit_payload(38000, own(repeat_pattern("100", 1000)))));
	/* 65535 * 26 = 1703910 is the last duration the driver can count at 38 kHz */
	add_case(mk("cycles-max", true, NULL, "38000,1703910"), "transmit", "{\"frequency\":38000,\"pattern\":[1703910]}");
	add_case(mk("cycles-over", false, "count", NULL), "transmit", "{\"frequency\":38000,\"pattern\":[1703936]}");
	add_case(mk("burst-ok", true, NULL, NULL), "transmit", "{\"frequency\":38000,\"pattern\":[1700000,1700000,1600000]}");
	add_case(mk("burst-over", false, "longer than a burst", NULL), "transmit", "{\"frequency\":38000,\"pattern\":[1700000,1700000,1700000]}");
	/* 1000 x "4999" is 5000+ characters but only 4.999 s: the page limit, not the burst one */
	add_case(mk("page-limit", false, "too long", NULL), "transmit", own(transmit_payload(38000, own(repeat_pattern("4999", 1000)))));
	add_case(mk("status-extra-keys", true, NULL, NULL), "getStatus", "{\"frequency\":1}");

	run_next_case();
}

static void phase_ack(void)
{
	/* The FPGA says no */
	write_result("0\n");
	next_phase = phase_faults;
	g_atomic_int_inc(&outstanding);
	expect(mk("nack", true, "\"acknowledged\":false", NULL), "transmit", "{\"frequency\":38000,\"pattern\":[560,560,560]}");
	phase_done_check();
}

static void after_faults(void)
{
	g_atomic_int_set(&fault, FAULT_NONE);
	write_result("1\n");
	phase_flood();
}

static void phase_faults_short(void);
static void phase_faults_one(void);

static void phase_faults(void)
{
	write_result("1\n");
	g_atomic_int_set(&fault, FAULT_EIO);
	next_phase = phase_faults_one;
	g_atomic_int_inc(&outstanding);
	expect(mk("eio", false, NULL, NULL), "transmit", "{\"frequency\":38000,\"pattern\":[560]}");
	phase_done_check();
}

static void phase_faults_one(void)
{
	g_atomic_int_set(&fault, FAULT_ONE);
	next_phase = phase_faults_short;
	g_atomic_int_inc(&outstanding);
	expect(mk("fw-missing", false, NULL, NULL), "transmit", "{\"frequency\":38000,\"pattern\":[560]}");
	phase_done_check();
}

static void phase_faults_short(void)
{
	g_atomic_int_set(&fault, FAULT_SHORT);
	next_phase = after_faults;
	g_atomic_int_inc(&outstanding);
	expect(mk("short-write", false, NULL, NULL), "transmit", "{\"frequency\":38000,\"pattern\":[560,560,560]}");
	phase_done_check();
}

static gint flood_ok = 0, flood_busy = 0;

static void on_flood(const char *request, const char *reply, gpointer user_data)
{
	if (strstr(reply, "\"returnValue\":true"))
		g_atomic_int_inc(&flood_ok);
	else if (strstr(reply, "busy"))
		g_atomic_int_inc(&flood_busy);
	else
		CHECK(false, "flood: unexpected reply %s", reply);

	phase_done_check();
}

static void flood_checked(void)
{
	/* One on the LED and MAX_QUEUED behind it went out; the rest were turned away */
	CHECK(flood_ok == MAX_QUEUED + 1, "flood: %d accepted, want %d", flood_ok, MAX_QUEUED + 1);
	CHECK(flood_ok + flood_busy == 40, "flood: %d+%d replies for 40 calls", flood_ok, flood_busy);
	phase_fuzz();
}

static void phase_flood(void)
{
	/*
	 * The worker is held in write() for the whole flood, so the queue can only
	 * fill: exactly MAX_QUEUED more get in, and the busy replies - which come
	 * back synchronously - are all there before the gate opens.
	 */
	gate_set(true);
	g_atomic_int_inc(&outstanding);
	shim_call("transmit", "{\"frequency\":38000,\"pattern\":[560,560,560]}", on_flood, NULL);
	gate_wait_held();

	next_phase = flood_checked;
	g_atomic_int_inc(&outstanding);

	for (int i = 1; i < 40; i++)
	{
		g_atomic_int_inc(&outstanding);
		shim_call("transmit", "{\"frequency\":38000,\"pattern\":[560,560,560]}", on_flood, NULL);
	}

	CHECK(g_atomic_int_get(&flood_busy) == 40 - (MAX_QUEUED + 1),
	      "flood: %d busy while the worker was held, want %d", flood_busy, 40 - (MAX_QUEUED + 1));
	gate_set(false);
	phase_done_check();
}

static gint fuzz_replies = 0;

static void on_fuzz(const char *request, const char *reply, gpointer user_data)
{
	g_atomic_int_inc(&fuzz_replies);
	CHECK(strstr(reply, "\"returnValue\"") != NULL, "fuzz: reply without returnValue: %s", reply);
	phase_done_check();
}

static void fuzz_checked(void)
{
	CHECK(fuzz_replies == 20000, "fuzz: %d replies for 20000 calls", fuzz_replies);
	phase_shutdown();
}

static void phase_fuzz(void)
{
	static const char *pieces[] = {
		"{", "}", "[", "]", ",", ":", "\"frequency\"", "\"pattern\"", "38000", "36000", "0", "-1",
		"560", "1690", "9000", "2147483647", "2147483648", "1e3", "0.5", "null", "true", "\"x\"",
		"\"\\u0041\"", " ", "\"subscribe\"", "1703910", "99999999999999999999",
	};
	GRand *rand = g_rand_new_with_seed(20261006);

	next_phase = fuzz_checked;
	g_atomic_int_inc(&outstanding);

	for (int i = 0; i < 20000; i++)
	{
		GString *s = g_string_new(NULL);
		int kind = g_rand_int_range(rand, 0, 3);

		if (kind == 0)
		{
			/* Random token soup */
			int n = g_rand_int_range(rand, 0, 30);
			for (int k = 0; k < n; k++)
				g_string_append(s, pieces[g_rand_int_range(rand, 0, G_N_ELEMENTS(pieces))]);
		}
		else if (kind == 1)
		{
			/* Well formed, values anywhere in and around the limits */
			int n = g_rand_int_range(rand, 0, 1100);
			g_string_append_printf(s, "{\"frequency\":%d,\"pattern\":[",
			                       g_rand_int_range(rand, 10000, 600000));
			for (int k = 0; k < n; k++)
				g_string_append_printf(s, "%s%d", k ? "," : "", g_rand_int_range(rand, -10, 2000000));
			g_string_append(s, "]}");
		}
		else
		{
			/* Random bytes */
			int n = g_rand_int_range(rand, 0, 64);
			for (int k = 0; k < n; k++)
				g_string_append_c(s, (char)g_rand_int_range(rand, 1, 256));
		}

		g_atomic_int_inc(&outstanding);
		shim_call(g_rand_boolean(rand) ? "transmit" : "getStatus", s->str, on_fuzz, NULL);
		g_string_free(s, TRUE);

		/* Let the worker drain now and then so the queue limit is not all we test */
		if (i % 50 == 0)
			while (g_main_context_iteration(NULL, FALSE));
	}

	g_rand_free(rand);
	phase_done_check();
}

static void on_dropped(const char *request, const char *reply, gpointer user_data)
{
	/* The burst on the LED is sent; the ones queued behind it get "shutting down" */
}

static void phase_shutdown(void)
{
	/* A disconnect with work queued: the queued jobs must be freed, not leaked */
	g_atomic_int_set(&slow_write_ms, 30);

	for (int i = 0; i < 6; i++)
		shim_call("transmit", "{\"frequency\":38000,\"pattern\":[560]}", on_dropped, NULL);

	shim_disconnect();
}

static void start_sec_ir(void)
{
	phase_validation();
}

/* lirc: same API, binary write of an odd number of unsigned ints */
static void lirc_checked(const char *request, const char *reply, gpointer user_data)
{
	char *data;

	CHECK(strstr(reply, "\"returnValue\":true") != NULL, "lirc: %s", reply);
	pthread_mutex_lock(&capture_lock);
	data = g_memdup2(captured->data, captured->len);
	CHECK(captured->len == 3 * sizeof(unsigned int), "lirc: wrote %u bytes, want 3 ints (even count trimmed)", captured->len);
	pthread_mutex_unlock(&capture_lock);
	CHECK(data && ((unsigned int *)data)[0] == 9000 && ((unsigned int *)data)[2] == 560, "lirc: wrong durations");
	/* Both set before the write; a driver may have no defaults (mtk_irtx_pwm) */
	CHECK(g_atomic_int_get(&lirc_carrier) == 38000, "lirc: carrier %d, want 38000", g_atomic_int_get(&lirc_carrier));
	CHECK(g_atomic_int_get(&lirc_duty) == 33, "lirc: duty cycle %d, want 33", g_atomic_int_get(&lirc_duty));
	g_free(data);
	shim_disconnect();
}

static void lirc_status(const char *request, const char *reply, gpointer user_data)
{
	CHECK(strstr(reply, "\"backend\":\"lirc\"") != NULL, "lirc status: %s", reply);
	shim_call("transmit", "{\"frequency\":38000,\"pattern\":[9000,4500,560,560]}", lirc_checked, NULL);
}

static void start_lirc(void)
{
	shim_call("getStatus", "{}", lirc_status, NULL);
}

static void none_reply(const char *request, const char *reply, gpointer user_data)
{
	if (strstr(request, "frequency"))
	{
		CHECK(strstr(reply, "no infrared transmitter") != NULL, "none: %s", reply);
		shim_disconnect();
	}
	else
	{
		CHECK(strstr(reply, "\"available\":false") != NULL, "none status: %s", reply);
	}
}

static void start_none(void)
{
	shim_call("getStatus", "{}", none_reply, NULL);
	shim_call("transmit", "{\"frequency\":38000,\"pattern\":[560]}", none_reply, NULL);
}

/* The first burst is on the LED when SIGTERM comes and goes out; the second is queued and told no */
static gint sigterm_sent = 0, sigterm_refused = 0;

static void sigterm_reply(const char *request, const char *reply, gpointer user_data)
{
	if (strstr(reply, "\"returnValue\":true"))
		g_atomic_int_inc(&sigterm_sent);
	else if (strstr(reply, "shutting down"))
		g_atomic_int_inc(&sigterm_refused);
	else
		CHECK(false, "sigterm: unexpected reply %s", reply);
}

static void start_sigterm(void)
{
	g_atomic_int_set(&slow_write_ms, 50);
	shim_call("transmit", "{\"frequency\":38000,\"pattern\":[560]}", sigterm_reply, NULL);
	shim_call("transmit", "{\"frequency\":38000,\"pattern\":[560]}", sigterm_reply, NULL);
	raise(SIGTERM);
}

/*
 * The descriptors that are files or devices, as "fd -> target" lines. glib's
 * own anon_inode eventfds live as long as the process does and are left out;
 * anything of ours that leaks is a real path, the sysfs node or lirc0.
 */
static char *open_files(void)
{
	GString *out = g_string_new(NULL);
	GDir *dir = g_dir_open("/proc/self/fd", 0, NULL);
	const char *name;

	while (dir && (name = g_dir_read_name(dir)))
	{
		char *link = g_strdup_printf("/proc/self/fd/%s", name);
		char *target = g_file_read_link(link, NULL);

		if (target && g_str_has_prefix(target, "/") && !g_str_has_prefix(target, "/proc/"))
			g_string_append_printf(out, "%s -> %s\n", name, target);

		g_free(target);
		g_free(link);
	}

	if (dir)
		g_dir_close(dir);

	return g_string_free(out, FALSE);
}

/*
 * late-reply: the worker finishes a burst while the main loop is still
 * running - so its reply is queued as an idle source - and the hub goes away
 * before that source is dispatched. The reply must still be sent, once, and
 * nothing may leak.
 */
static gint late_replies = 0;

static void late_reply(const char *request, const char *reply, gpointer user_data)
{
	g_atomic_int_inc(&late_replies);
	CHECK(strstr(reply, "\"returnValue\":true") != NULL, "late-reply: %s", reply);
}

static void start_late_reply(void)
{
	shim_call("transmit", "{\"frequency\":38000,\"pattern\":[560,560,560]}", late_reply, NULL);

	/* Still inside a dispatch on the main thread, so the context stays ours */
	pthread_mutex_lock(&invoke_lock);
	while (invokes == 0)
		pthread_cond_wait(&invoke_cond, &invoke_lock);
	pthread_mutex_unlock(&invoke_lock);

	CHECK(g_atomic_int_get(&late_replies) == 0, "late-reply: reply dispatched before the disconnect");
	shim_disconnect();
}

int main(int argc, char **argv)
{
	int status;
	char *files_before, *files_after;

	mode = argc > 1 ? argv[1] : "sec_ir";
	captured = g_byte_array_new();
	keep = g_ptr_array_new_with_free_func(g_free);

	/* The nodes main.c probes: present or not, by mode */
	g_unlink(SEC_IR_SEND);
	g_unlink(LIRC_DEVICE);
	write_result("1\n");

	if (strcmp(mode, "lirc") == 0)
	{
		g_assert(g_file_set_contents(LIRC_DEVICE, "", 0, NULL));
		shim_set_start(start_lirc);
	}
	else if (strcmp(mode, "none") == 0)
	{
		shim_set_start(start_none);
	}
	else if (strcmp(mode, "late-reply") == 0)
	{
		g_assert(g_file_set_contents(SEC_IR_SEND, "", 0, NULL));
		shim_set_start(start_late_reply);
	}
	else
	{
		g_assert(g_file_set_contents(SEC_IR_SEND, "", 0, NULL));
		shim_set_start(strcmp(mode, "sigterm") == 0 ? start_sigterm : start_sec_ir);
	}

	files_before = open_files();
	status = irblasterd_main(0, NULL);
	files_after = open_files();
	CHECK(strcmp(files_before, files_after) == 0, "%s: file descriptors left open:\n%s\nwas:\n%s",
	      mode, files_after, files_before);
	g_free(files_before);
	g_free(files_after);

	if (strcmp(mode, "sigterm") == 0)
	{
		CHECK(status == 0, "sigterm: exit status %d, want 0", status);
		/* Bumped on the worker, which replied inline during the shutdown */
		int sent = g_atomic_int_get(&sigterm_sent), refused = g_atomic_int_get(&sigterm_refused);
		CHECK(sent == 1 && refused == 1, "sigterm: %d sent and %d refused, want 1 and 1", sent, refused);
	}
	else
		CHECK(status == 1, "%s: exit status %d after a disconnect, want 1", mode, status);

	if (strcmp(mode, "late-reply") == 0)
		CHECK(g_atomic_int_get(&late_replies) == 1, "late-reply: %d replies, want 1", g_atomic_int_get(&late_replies));

	CHECK(shim_live_messages() == 0, "%s: %d messages never released", mode, shim_live_messages());
	CHECK(shim_live_values() == 0, "%s: %d JSON values never released", mode, shim_live_values());
	CHECK(shim_bad_replies() == 0, "%s: %d bad replies", mode, shim_bad_replies());

	g_ptr_array_free(keep, TRUE);
	g_byte_array_free(captured, TRUE);

	if (failures)
	{
		g_printerr("%s: %d failures\n", mode, failures);
		return 1;
	}

	g_print("%s: all checks passed\n", mode);
	return 0;
}
