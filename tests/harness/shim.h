/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <luna-service2/lunaservice.h>
#include <pbnjson.h>

typedef void (*ShimStartFunc)(void);
typedef void (*ShimReplyFunc)(const char *request, const char *reply, gpointer user_data);

/* Called once the service's main loop runs */
void shim_set_start(ShimStartFunc func);
/* Delivers a call as the hub would: one reference, dropped when the handler returns */
bool shim_call(const char *method, const char *payload, ShimReplyFunc on_reply, gpointer user_data);
void shim_disconnect(void);
int shim_live_values(void);
int shim_live_messages(void);
int shim_bad_replies(void);
/* Statuses pushed to subscribers so far, "key payload" per line (g_free it) */
char *shim_posts(void);
void shim_free_posts(void);
