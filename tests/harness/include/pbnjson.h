/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test stand-in for the slice of pbnjson's C API irblasterd uses, with the
 * same ownership rules: *_create and jdom_parse return an owned reference,
 * jobject_put takes ownership of key and value, jobject_get_exists and
 * jarray_get lend one. Every value is a separate heap object freed on its
 * last release, so a release of a borrowed reference is a double free that
 * ASan and valgrind catch.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

typedef struct jvalue *jvalue_ref;
typedef struct { const char *m_str; size_t m_len; } raw_buffer;
typedef struct jschema *jschema_ref;
typedef struct { jschema_ref m_schema; } JSchemaInfo;
typedef enum { CONV_OK = 0, CONV_POSITIVE_OVERFLOW = 0x1, CONV_NEGATIVE_OVERFLOW = 0x2,
               CONV_PRECISION_LOSS = 0x4, CONV_NOT_A_NUM = 0x8 } ConversionResultFlags;
typedef enum { DOMOPT_NOOPT = 0 } JDOMOptimizationFlags;

raw_buffer j_cstr_to_buffer(const char *str);
#define J_CSTR_TO_BUF(s) j_cstr_to_buffer(s)
#define J_CSTR_TO_JVAL(s) jstring_create(s)

jvalue_ref jobject_create(void);
jvalue_ref jboolean_create(bool value);
jvalue_ref jstring_create(const char *str);
jvalue_ref jnumber_create_i32(int32_t value);
bool jobject_put(jvalue_ref obj, jvalue_ref key, jvalue_ref val);
bool jobject_get_exists(jvalue_ref obj, raw_buffer key, jvalue_ref *val);
const char *jvalue_tostring_simple(jvalue_ref val);
void j_release(jvalue_ref *val);
bool jis_null(jvalue_ref val);
bool jis_number(jvalue_ref val);
bool jis_array(jvalue_ref val);
ssize_t jarray_size(jvalue_ref arr);
jvalue_ref jarray_get(jvalue_ref arr, ssize_t index);
ConversionResultFlags jnumber_get_i32(jvalue_ref num, int32_t *out);
jschema_ref jschema_all(void);
void jschema_info_init(JSchemaInfo *info, jschema_ref schema, void *resolver, void *handler);
jvalue_ref jdom_parse(raw_buffer input, JDOMOptimizationFlags opts, JSchemaInfo *info);
