/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef APP_WEBRTC_H_
#define APP_WEBRTC_H_

#define APP_WEBRTC_TASK_PRIORITY    (2)
#define APP_WEBRTC_TASK_STACK       (8U * 1024U)

/* Spawn the WebRTC task. Call after Wi-Fi up + NTP synced + iotconnect_sdk_init.
 * Non-blocking: returns immediately so the caller's loop (telemetry etc.) keeps running. */
void app_webrtc_start(void);

#endif /* APP_WEBRTC_H_ */
