/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test stand-in for the slice of luna-service2 irblasterd uses. Same names
 * and signatures as the real header; behaviour lives in shim.c.
 */
#pragma once
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct LSHandle LSHandle;
typedef struct LSMessage LSMessage;
typedef struct { int error_code; char *message; } LSError;
typedef bool (*LSMethodFunction)(LSHandle *sh, LSMessage *msg, void *ctx);
typedef void (*LSDisconnectHandler)(LSHandle *sh, void *ctx);
typedef enum { LUNA_METHOD_FLAGS_NONE = 0 } LSMethodFlags;
typedef struct { const char *name; LSMethodFunction function; LSMethodFlags flags; } LSMethod;

bool LSErrorInit(LSError *error);
void LSErrorFree(LSError *error);
void LSErrorPrint(LSError *error, FILE *out);
bool LSRegister(const char *name, LSHandle **sh, LSError *error);
bool LSRegisterCategory(LSHandle *sh, const char *category, LSMethod *methods,
                        void *signals, void *properties, LSError *error);
bool LSGmainAttach(LSHandle *sh, GMainLoop *loop, LSError *error);
bool LSSetDisconnectHandler(LSHandle *sh, LSDisconnectHandler handler, void *ctx, LSError *error);
bool LSUnregister(LSHandle *sh, LSError *error);
bool LSMessageReply(LSHandle *sh, LSMessage *msg, const char *payload, LSError *error);
const char *LSMessageGetPayload(LSMessage *msg);
bool LSMessageIsSubscription(LSMessage *msg);
bool LSSubscriptionReply(LSHandle *sh, const char *key, const char *payload, LSError *error);
bool LSSubscriptionProcess(LSHandle *sh, LSMessage *msg, bool *subscribed, LSError *error);
void LSMessageRef(LSMessage *msg);
void LSMessageUnref(LSMessage *msg);
