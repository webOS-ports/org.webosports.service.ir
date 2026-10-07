/*
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Controls and observations of the stand-in libgbinder (fakegbinder.c).
 */
#pragma once
#include <glib.h>

void fake_gbinder_set_present(int value);
void fake_gbinder_fail_next(int count);
void fake_gbinder_set_reply(int success, int status);
int fake_gbinder_last(guint32 *carrier, gint32 *pattern, guint max);
int fake_gbinder_transacts(void);
int fake_gbinder_overlaps(void);
/* Service managers, clients, requests and replies not released */
int fake_gbinder_live(void);
