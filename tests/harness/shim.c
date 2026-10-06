/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Behaviour behind the stand-in luna-service2 and pbnjson headers. Strict on
 * purpose: ownership mistakes become use-after-free or double free for the
 * sanitizers, every message must be answered exactly once, from the thread
 * that owns the main context, and nothing may be left alive at exit.
 */

#include "shim.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- pbnjson */

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } JvType;

struct jvalue
{
	JvType type;
	gint refs;
	bool b;
	bool integral;      /* the number was written without fraction/exponent */
	gint64 i;
	double d;
	char *s;
	GPtrArray *items;   /* arrays: values; objects: key, value, key, value... */
	char *text;         /* jvalue_tostring_simple's result, owned here */
};

static gint live_values = 0;

static jvalue_ref jv_new(JvType type)
{
	jvalue_ref v = g_new0(struct jvalue, 1);
	v->type = type;
	v->refs = 1;
	g_atomic_int_inc(&live_values);
	return v;
}

static void jv_unref(jvalue_ref v)
{
	if (!v || !g_atomic_int_dec_and_test(&v->refs))
	{
		return;
	}

	if (v->items)
	{
		for (guint k = 0; k < v->items->len; k++)
		{
			jv_unref(g_ptr_array_index(v->items, k));
		}

		g_ptr_array_free(v->items, TRUE);
	}

	g_free(v->s);
	g_free(v->text);
	g_free(v);
	g_atomic_int_add(&live_values, -1);
}

raw_buffer j_cstr_to_buffer(const char *str)
{
	raw_buffer b = { str, str ? strlen(str) : 0 };
	return b;
}

jvalue_ref jobject_create(void)
{
	jvalue_ref v = jv_new(JV_OBJ);
	v->items = g_ptr_array_new();
	return v;
}

jvalue_ref jboolean_create(bool value)
{
	jvalue_ref v = jv_new(JV_BOOL);
	v->b = value;
	return v;
}

jvalue_ref jstring_create(const char *str)
{
	jvalue_ref v = jv_new(JV_STR);
	v->s = g_strdup(str);
	return v;
}

jvalue_ref jnumber_create_i32(int32_t value)
{
	jvalue_ref v = jv_new(JV_NUM);
	v->integral = true;
	v->i = value;
	v->d = value;
	return v;
}

bool jobject_put(jvalue_ref obj, jvalue_ref key, jvalue_ref val)
{
	g_assert(obj && obj->type == JV_OBJ && key && key->type == JV_STR && val);
	g_ptr_array_add(obj->items, key);
	g_ptr_array_add(obj->items, val);
	return true;
}

bool jobject_get_exists(jvalue_ref obj, raw_buffer key, jvalue_ref *val)
{
	if (!obj || obj->type != JV_OBJ)
	{
		return false;
	}

	for (guint k = 0; k + 1 < obj->items->len; k += 2)
	{
		jvalue_ref name = g_ptr_array_index(obj->items, k);

		if (strlen(name->s) == key.m_len && memcmp(name->s, key.m_str, key.m_len) == 0)
		{
			*val = g_ptr_array_index(obj->items, k + 1);   /* borrowed */
			return true;
		}
	}

	return false;
}

static void jv_print(GString *out, jvalue_ref v)
{
	switch (v->type)
	{
	case JV_NULL: g_string_append(out, "null"); break;
	case JV_BOOL: g_string_append(out, v->b ? "true" : "false"); break;
	case JV_NUM:
		if (v->integral)
			g_string_append_printf(out, "%" G_GINT64_FORMAT, v->i);
		else
			g_string_append_printf(out, "%.17g", v->d);
		break;
	case JV_STR:
	{
		char *e = g_strescape(v->s, NULL);
		g_string_append_printf(out, "\"%s\"", e);
		g_free(e);
		break;
	}
	case JV_ARR:
		g_string_append_c(out, '[');
		for (guint k = 0; k < v->items->len; k++)
		{
			if (k) g_string_append_c(out, ',');
			jv_print(out, g_ptr_array_index(v->items, k));
		}
		g_string_append_c(out, ']');
		break;
	case JV_OBJ:
		g_string_append_c(out, '{');
		for (guint k = 0; k + 1 < v->items->len; k += 2)
		{
			if (k) g_string_append_c(out, ',');
			jv_print(out, g_ptr_array_index(v->items, k));
			g_string_append_c(out, ':');
			jv_print(out, g_ptr_array_index(v->items, k + 1));
		}
		g_string_append_c(out, '}');
		break;
	}
}

const char *jvalue_tostring_simple(jvalue_ref val)
{
	GString *out = g_string_new(NULL);
	jv_print(out, val);
	g_free(val->text);
	val->text = g_string_free(out, FALSE);
	return val->text;
}

void j_release(jvalue_ref *val)
{
	g_assert(val != NULL);
	jv_unref(*val);
	*val = NULL;
}

bool jis_null(jvalue_ref val) { return !val || val->type == JV_NULL; }
bool jis_number(jvalue_ref val) { return val && val->type == JV_NUM; }
bool jis_array(jvalue_ref val) { return val && val->type == JV_ARR; }

ssize_t jarray_size(jvalue_ref arr)
{
	return (arr && arr->type == JV_ARR) ? (ssize_t)arr->items->len : -1;
}

jvalue_ref jarray_get(jvalue_ref arr, ssize_t index)
{
	g_assert(arr && arr->type == JV_ARR && index >= 0 && (guint)index < arr->items->len);
	return g_ptr_array_index(arr->items, (guint)index);   /* borrowed */
}

ConversionResultFlags jnumber_get_i32(jvalue_ref num, int32_t *out)
{
	if (!num || num->type != JV_NUM)
		return CONV_NOT_A_NUM;

	if (!num->integral)
	{
		*out = (int32_t)num->d;
		return CONV_PRECISION_LOSS;
	}

	if (num->i > INT32_MAX)
	{
		*out = INT32_MAX;
		return CONV_POSITIVE_OVERFLOW;
	}

	if (num->i < INT32_MIN)
	{
		*out = INT32_MIN;
		return CONV_NEGATIVE_OVERFLOW;
	}

	*out = (int32_t)num->i;
	return CONV_OK;
}

jschema_ref jschema_all(void) { return NULL; }

void jschema_info_init(JSchemaInfo *info, jschema_ref schema, void *resolver, void *handler)
{
	info->m_schema = schema;
}

/* A small strict JSON reader; anything it cannot read becomes null */
typedef struct { const char *p; const char *end; int depth; } Reader;

static void skip_ws(Reader *r)
{
	while (r->p < r->end && isspace((unsigned char)*r->p))
		r->p++;
}

static jvalue_ref parse_value(Reader *r);

static char *parse_string(Reader *r)
{
	GString *s = g_string_new(NULL);

	r->p++;   /* opening quote */

	while (r->p < r->end && *r->p != '"')
	{
		char c = *r->p++;

		if (c == '\\')
		{
			if (r->p >= r->end)
				goto bad;

			c = *r->p++;

			switch (c)
			{
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'u':
				if (r->end - r->p < 4)
					goto bad;
				r->p += 4;
				c = '?';
				break;
			case '"': case '\\': case '/': break;
			default: goto bad;
			}
		}
		else if ((unsigned char)c < 0x20)
		{
			goto bad;
		}

		g_string_append_c(s, c);
	}

	if (r->p >= r->end)
		goto bad;

	r->p++;   /* closing quote */
	return g_string_free(s, FALSE);

bad:
	g_string_free(s, TRUE);
	return NULL;
}

static jvalue_ref parse_number(Reader *r)
{
	const char *start = r->p;
	bool integral = true;
	char *text;
	jvalue_ref v;

	if (r->p < r->end && *r->p == '-')
		r->p++;

	if (r->p >= r->end || !isdigit((unsigned char)*r->p))
		return NULL;

	while (r->p < r->end && (isdigit((unsigned char)*r->p) || strchr(".eE+-", *r->p)))
	{
		if (strchr(".eE", *r->p))
			integral = false;
		r->p++;
	}

	text = g_strndup(start, (gsize)(r->p - start));
	v = jv_new(JV_NUM);
	errno = 0;

	if (integral)
	{
		v->i = g_ascii_strtoll(text, NULL, 10);
		v->integral = errno == 0;
		v->d = (double)v->i;

		if (!v->integral)
			v->d = g_ascii_strtod(text, NULL);
	}
	else
	{
		v->d = g_ascii_strtod(text, NULL);
	}

	g_free(text);
	return v;
}

static jvalue_ref parse_container(Reader *r, bool object)
{
	jvalue_ref v = object ? jobject_create() : jv_new(JV_ARR);

	if (!object)
		v->items = g_ptr_array_new();

	if (++r->depth > 64)
		goto bad;

	r->p++;
	skip_ws(r);

	if (r->p < r->end && *r->p == (object ? '}' : ']'))
	{
		r->p++;
		r->depth--;
		return v;
	}

	for (;;)
	{
		jvalue_ref item;

		skip_ws(r);

		if (object)
		{
			char *key;

			if (r->p >= r->end || *r->p != '"' || !(key = parse_string(r)))
				goto bad;

			g_ptr_array_add(v->items, jstring_create(key));
			g_free(key);
			skip_ws(r);

			if (r->p >= r->end || *r->p != ':')
			{
				g_ptr_array_add(v->items, jv_new(JV_NULL));
				goto bad;
			}

			r->p++;
		}

		if (!(item = parse_value(r)))
		{
			if (object)
				g_ptr_array_add(v->items, jv_new(JV_NULL));
			goto bad;
		}

		g_ptr_array_add(v->items, item);
		skip_ws(r);

		if (r->p < r->end && *r->p == ',')
		{
			r->p++;
			continue;
		}

		if (r->p < r->end && *r->p == (object ? '}' : ']'))
		{
			r->p++;
			r->depth--;
			return v;
		}

		goto bad;
	}

bad:
	jv_unref(v);
	return NULL;
}

static jvalue_ref parse_value(Reader *r)
{
	skip_ws(r);

	if (r->p >= r->end)
		return NULL;

	switch (*r->p)
	{
	case '{': return parse_container(r, true);
	case '[': return parse_container(r, false);
	case '"':
	{
		char *s = parse_string(r);
		jvalue_ref v;

		if (!s)
			return NULL;
		v = jstring_create(s);
		g_free(s);
		return v;
	}
	case 't':
		if (r->end - r->p >= 4 && memcmp(r->p, "true", 4) == 0) { r->p += 4; return jboolean_create(true); }
		return NULL;
	case 'f':
		if (r->end - r->p >= 5 && memcmp(r->p, "false", 5) == 0) { r->p += 5; return jboolean_create(false); }
		return NULL;
	case 'n':
		if (r->end - r->p >= 4 && memcmp(r->p, "null", 4) == 0) { r->p += 4; return jv_new(JV_NULL); }
		return NULL;
	default:
		return parse_number(r);
	}
}

jvalue_ref jdom_parse(raw_buffer input, JDOMOptimizationFlags opts, JSchemaInfo *info)
{
	Reader r = { input.m_str, input.m_str + input.m_len, 0 };
	jvalue_ref v = input.m_str ? parse_value(&r) : NULL;

	if (v)
	{
		skip_ws(&r);

		if (r.p != r.end)
		{
			jv_unref(v);
			v = NULL;
		}
	}

	/* Like pbnjson, a failed parse is an owned value that jis_null() accepts */
	return v ? v : jv_new(JV_NULL);
}

/* ---------------------------------------------------------- luna-service2 */

struct LSHandle { int dummy; };

struct LSMessage
{
	gint refs;
	char *payload;
	gint replies;
	char *reply;
	ShimReplyFunc on_reply;
	gpointer user_data;
};

static struct LSHandle the_handle;
static LSMethod *registered_methods = NULL;
static LSDisconnectHandler disconnect_handler = NULL;
static ShimStartFunc start_func = NULL;
static gint live_messages = 0;
static gint bad_replies = 0;

bool LSErrorInit(LSError *error) { error->error_code = 0; error->message = NULL; return true; }
void LSErrorFree(LSError *error) { g_free(error->message); error->message = NULL; }
void LSErrorPrint(LSError *error, FILE *out) { fprintf(out, "LSError: %s\n", error->message ? error->message : "?"); }

bool LSRegister(const char *name, LSHandle **sh, LSError *error)
{
	*sh = &the_handle;
	return true;
}

bool LSRegisterCategory(LSHandle *sh, const char *category, LSMethod *methods,
                        void *signals, void *properties, LSError *error)
{
	registered_methods = methods;
	return true;
}

static gboolean run_start(gpointer data)
{
	if (start_func)
		start_func();
	return G_SOURCE_REMOVE;
}

bool LSGmainAttach(LSHandle *sh, GMainLoop *loop, LSError *error)
{
	g_idle_add(run_start, NULL);
	return true;
}

bool LSSetDisconnectHandler(LSHandle *sh, LSDisconnectHandler handler, void *ctx, LSError *error)
{
	disconnect_handler = handler;
	return true;
}

bool LSUnregister(LSHandle *sh, LSError *error)
{
	return true;
}

bool LSMessageReply(LSHandle *sh, LSMessage *msg, const char *payload, LSError *error)
{
	/* Replies must come from whoever owns the main context right now */
	if (!g_main_context_is_owner(g_main_context_default()))
	{
		g_printerr("FAIL: reply from a thread that does not own the main context\n");
		g_atomic_int_inc(&bad_replies);
	}

	if (g_atomic_int_add(&msg->replies, 1) != 0)
	{
		g_printerr("FAIL: second reply to one message: %s\n", payload);
		g_atomic_int_inc(&bad_replies);
	}

	g_free(msg->reply);
	msg->reply = g_strdup(payload);

	if (msg->on_reply)
		msg->on_reply(msg->payload, payload, msg->user_data);

	return true;
}

const char *LSMessageGetPayload(LSMessage *msg) { return msg->payload; }
bool LSMessageIsSubscription(LSMessage *msg) { return strstr(msg->payload, "\"subscribe\":true") != NULL; }

bool LSSubscriptionProcess(LSHandle *sh, LSMessage *msg, bool *subscribed, LSError *error)
{
	*subscribed = true;
	return true;
}

void LSMessageRef(LSMessage *msg)
{
	g_atomic_int_inc(&msg->refs);
}

void LSMessageUnref(LSMessage *msg)
{
	if (!g_atomic_int_dec_and_test(&msg->refs))
		return;

	g_free(msg->payload);
	g_free(msg->reply);
	g_free(msg);
	g_atomic_int_add(&live_messages, -1);
}

/* ----------------------------------------------------------- test driving */

void shim_set_start(ShimStartFunc func)
{
	start_func = func;
}

bool shim_call(const char *method, const char *payload, ShimReplyFunc on_reply, gpointer user_data)
{
	LSMessage *msg = g_new0(LSMessage, 1);
	bool handled = false;

	msg->refs = 1;
	msg->payload = g_strdup(payload);
	msg->on_reply = on_reply;
	msg->user_data = user_data;
	g_atomic_int_inc(&live_messages);

	for (LSMethod *m = registered_methods; m && m->name; m++)
	{
		if (strcmp(m->name, method) == 0)
		{
			handled = m->function(&the_handle, msg, NULL);
			break;
		}
	}

	/* The hub's own reference, dropped once the handler has returned */
	LSMessageUnref(msg);
	return handled;
}

void shim_disconnect(void)
{
	g_assert(disconnect_handler != NULL);
	disconnect_handler(&the_handle, NULL);
}

int shim_live_values(void) { return g_atomic_int_get(&live_values); }
int shim_live_messages(void) { return g_atomic_int_get(&live_messages); }
int shim_bad_replies(void) { return g_atomic_int_get(&bad_replies); }
