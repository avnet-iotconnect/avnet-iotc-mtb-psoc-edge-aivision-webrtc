/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef APP_WEBRTC_H_
#define APP_WEBRTC_H_

#include <stdbool.h>

/* Lifecycle mirrors app_shmem_video: init at boot (idle task), start after
 * pre-reqs are met, stop on teardown. The H.264 ring consumer is owned by
 * this task once a peer connection is up (DTLS keyed); do not also run
 * app_shmem_video while WebRTC is active — the ring has a single consumer.
 *
 * Device acts as KVS viewer. "start" = connect to the signaling channel and
 * wait for a master to initiate. Media only flows after the master sends an
 * SDP offer and the ICE/DTLS handshake completes; the ring consumer starts
 * at that point, not at app_webrtc_start(). A signaling session can host
 * zero, one, or many viewing events from the master.
 */

// Create the WebRTC task in idle state. Called once from main.c at boot.
// Idempotent.
void app_webrtc_init(void);

// Bring the device onto the signaling channel as viewer and keep it there.
// Does NOT imply media is flowing — that is master-initiated. Caller must
// have ensured iotconnect_sdk_obtain_aws_creds() succeeded and
// iotcl_mqtt_get_config()->aws.webrtc_channel_arn is non-NULL. Non-blocking:
// flips a flag and returns; the task picks up work on its next tick.
// Returns true if start was accepted (or the task is already running).
bool app_webrtc_start(void);

// Drop any active master-initiated viewing event, leave the signaling
// channel, and return the task to idle. Synchronous with a 5 s timeout
// (logs and returns anyway on timeout). Idempotent.
void app_webrtc_stop(void);

// Plug point for cred refresh: caller invokes this after successfully
// re-running iotconnect_sdk_obtain_aws_creds(). Currently sets a latch the
// task observes at session boundary; no behavior change yet. Wired now to
// keep the refresh integration story straight from day one.
void app_webrtc_notify_creds_updated(void);

#endif // APP_WEBRTC_H_
