/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

/* Linkage / smoke tests for the WebRTC third-party libraries integrated in
 * kvs_libs.mk.  Call webrtc_smoke_test_run() once at startup (e.g. from
 * app_task.c after WiFi is connected, but it needs no network — all tests are
 * pure computation).
 */

#ifndef WEBRTC_SMOKE_TEST_H
#define WEBRTC_SMOKE_TEST_H

/* Returns the number of failed tests (0 = all passed). */
int webrtc_smoke_test_run(void);

#endif /* WEBRTC_SMOKE_TEST_H */
