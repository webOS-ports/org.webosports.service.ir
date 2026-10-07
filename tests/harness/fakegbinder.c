/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A stand-in libgbinder for the Android IR HAL backend: the fifteen calls
 * irblasterd resolves with dlsym, playing an android.hardware.ir@1.0
 * IConsumerIr that the tests can make absent, late, dead or unwilling.
 *
 * Strict where it counts: the hidl_vec is only read when the transaction is
 * made, so a pattern freed too early is a use-after-free for ASan; clients,
 * requests and replies are counted so a leak shows; and two threads inside it
 * at once is recorded, since irblasterd promises to serialise every call.
 */

#include <glib.h>
#include <string.h>

#include "fakegbinder.h"

typedef struct { gconstpointer d[4]; } GBinderWriter;
typedef struct { gconstpointer d[6]; } GBinderReader;

typedef struct { int dummy; } GBinderServiceManager;
typedef struct { int dummy; } GBinderRemoteObject;
/* A client whose HAL died stays dead: only a fresh lookup gets a live one */
typedef struct { gint dead; } GBinderClient;

typedef struct
{
	guint32 carrier;
	const gint32 *vec;      /* the caller's buffer, read at transact time */
	guint count;
	guint ints;
} GBinderLocalRequest;

typedef struct
{
	gint32 tx_status;
	gboolean success;
} GBinderRemoteReply;

#define IFACE "android.hardware.ir@1.0::IConsumerIr"

static GBinderServiceManager the_sm;
static GBinderRemoteObject the_remote;

static gint present = 1;
static gint fail_next = 0;
static gint reply_success = 1;
static gint reply_status = 0;

static gint live_sm = 0, live_clients = 0, live_requests = 0, live_replies = 0;
static gint transacts = 0, inside = 0, overlaps = 0;

static GMutex last_lock;
static guint32 last_carrier = 0;
static gint32 last_pattern[1024];
static guint last_count = 0;

static void enter(void)
{
	if (g_atomic_int_add(&inside, 1) != 0)
		g_atomic_int_inc(&overlaps);
}

static void leave(void)
{
	g_atomic_int_add(&inside, -1);
}

GBinderServiceManager *gbinder_servicemanager_new(const char *dev);
void gbinder_servicemanager_unref(GBinderServiceManager *sm);
GBinderRemoteObject *gbinder_servicemanager_get_service_sync(GBinderServiceManager *sm,
        const char *name, int *status);
GBinderClient *gbinder_client_new(GBinderRemoteObject *object, const char *iface);
void gbinder_client_unref(GBinderClient *client);
GBinderLocalRequest *gbinder_client_new_request(GBinderClient *client);
GBinderRemoteReply *gbinder_client_transact_sync_reply(GBinderClient *client, guint32 code,
        GBinderLocalRequest *req, int *status);
void gbinder_local_request_init_writer(GBinderLocalRequest *request, GBinderWriter *writer);
void gbinder_local_request_unref(GBinderLocalRequest *request);
void gbinder_writer_append_int32(GBinderWriter *writer, guint32 value);
void gbinder_writer_append_hidl_vec(GBinderWriter *writer, const void *base, guint count, guint elemsize);
void gbinder_remote_reply_init_reader(GBinderRemoteReply *reply, GBinderReader *reader);
gboolean gbinder_reader_read_int32(GBinderReader *reader, gint32 *value);
gboolean gbinder_reader_read_bool(GBinderReader *reader, gboolean *value);
void gbinder_remote_reply_unref(GBinderRemoteReply *reply);

GBinderServiceManager *gbinder_servicemanager_new(const char *dev)
{
	g_assert(g_strcmp0(dev, "/dev/hwbinder") == 0);
	g_atomic_int_inc(&live_sm);
	return &the_sm;
}

void gbinder_servicemanager_unref(GBinderServiceManager *sm)
{
	g_assert(sm == &the_sm);
	g_atomic_int_add(&live_sm, -1);
}

GBinderRemoteObject *gbinder_servicemanager_get_service_sync(GBinderServiceManager *sm,
        const char *name, int *status)
{
	GBinderRemoteObject *r = NULL;

	enter();
	g_assert(sm == &the_sm);
	g_assert(g_strcmp0(name, IFACE "/default") == 0);
	*status = 0;

	if (g_atomic_int_get(&present))
		r = &the_remote;   /* autoreleased: the caller does not unref it */

	leave();
	return r;
}

GBinderClient *gbinder_client_new(GBinderRemoteObject *object, const char *iface)
{
	g_assert(object == &the_remote);
	g_assert(g_strcmp0(iface, IFACE) == 0);
	g_atomic_int_inc(&live_clients);
	return g_new0(GBinderClient, 1);
}

void gbinder_client_unref(GBinderClient *client)
{
	g_atomic_int_add(&live_clients, -1);
	g_free(client);
}

GBinderLocalRequest *gbinder_client_new_request(GBinderClient *client)
{
	g_assert(client != NULL);
	g_atomic_int_inc(&live_requests);
	return g_new0(GBinderLocalRequest, 1);
}

void gbinder_local_request_init_writer(GBinderLocalRequest *request, GBinderWriter *writer)
{
	memset(writer, 0, sizeof(*writer));
	writer->d[0] = request;
}

void gbinder_local_request_unref(GBinderLocalRequest *request)
{
	g_atomic_int_add(&live_requests, -1);
	g_free(request);
}

void gbinder_writer_append_int32(GBinderWriter *writer, guint32 value)
{
	GBinderLocalRequest *req = (GBinderLocalRequest *)writer->d[0];

	req->carrier = value;
	req->ints++;
}

void gbinder_writer_append_hidl_vec(GBinderWriter *writer, const void *base, guint count, guint elemsize)
{
	GBinderLocalRequest *req = (GBinderLocalRequest *)writer->d[0];

	g_assert(elemsize == sizeof(gint32));
	req->vec = base;
	req->count = count;
}

GBinderRemoteReply *gbinder_client_transact_sync_reply(GBinderClient *client, guint32 code,
        GBinderLocalRequest *req, int *status)
{
	GBinderRemoteReply *reply;

	enter();
	g_assert(client != NULL && code == 1);
	g_assert(req->ints == 1 && req->count > 0 && req->count <= G_N_ELEMENTS(last_pattern));
	g_atomic_int_inc(&transacts);

	/* A proxy whose HAL died: this call and every later one on it fail, as with DEAD_OBJECT */
	if (g_atomic_int_get(&fail_next) > 0)
	{
		g_atomic_int_add(&fail_next, -1);
		g_atomic_int_set(&client->dead, 1);
	}

	if (g_atomic_int_get(&client->dead))
	{
		*status = -32;
		leave();
		return NULL;
	}

	g_mutex_lock(&last_lock);
	last_carrier = req->carrier;
	last_count = req->count;
	/* Read now, from the caller's buffer: it must still be alive */
	memcpy(last_pattern, req->vec, req->count * sizeof(gint32));
	g_mutex_unlock(&last_lock);

	reply = g_new0(GBinderRemoteReply, 1);
	reply->tx_status = g_atomic_int_get(&reply_status);
	reply->success = g_atomic_int_get(&reply_success);
	g_atomic_int_inc(&live_replies);
	*status = 0;
	leave();
	return reply;
}

void gbinder_remote_reply_init_reader(GBinderRemoteReply *reply, GBinderReader *reader)
{
	memset(reader, 0, sizeof(*reader));
	reader->d[0] = reply;
	reader->d[1] = GINT_TO_POINTER(0);
}

gboolean gbinder_reader_read_int32(GBinderReader *reader, gint32 *value)
{
	const GBinderRemoteReply *reply = reader->d[0];

	g_assert(GPOINTER_TO_INT(reader->d[1]) == 0);   /* the status comes first */
	*value = reply->tx_status;
	reader->d[1] = GINT_TO_POINTER(1);
	return TRUE;
}

gboolean gbinder_reader_read_bool(GBinderReader *reader, gboolean *value)
{
	const GBinderRemoteReply *reply = reader->d[0];

	g_assert(GPOINTER_TO_INT(reader->d[1]) == 1);   /* then the bool */
	*value = reply->success;
	reader->d[1] = GINT_TO_POINTER(2);
	return TRUE;
}

void gbinder_remote_reply_unref(GBinderRemoteReply *reply)
{
	g_atomic_int_add(&live_replies, -1);
	g_free(reply);
}

/* ---------------------------------------------------------- test controls */

void fake_gbinder_set_present(int value) { g_atomic_int_set(&present, value); }
void fake_gbinder_fail_next(int count) { g_atomic_int_set(&fail_next, count); }
void fake_gbinder_set_reply(int success, int status)
{
	g_atomic_int_set(&reply_success, success);
	g_atomic_int_set(&reply_status, status);
}

int fake_gbinder_last(guint32 *carrier, gint32 *pattern, guint max)
{
	guint n;

	g_mutex_lock(&last_lock);
	*carrier = last_carrier;
	n = MIN(last_count, max);
	memcpy(pattern, last_pattern, n * sizeof(gint32));
	g_mutex_unlock(&last_lock);
	return (int)n;
}

int fake_gbinder_transacts(void) { return g_atomic_int_get(&transacts); }
int fake_gbinder_overlaps(void) { return g_atomic_int_get(&overlaps); }

int fake_gbinder_live(void)
{
	return g_atomic_int_get(&live_sm) + g_atomic_int_get(&live_clients) +
	       g_atomic_int_get(&live_requests) + g_atomic_int_get(&live_replies);
}
