/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * CM33-NS WebRTC application flow. Owns the lifecycle, the run_session loop,
 * and the H.264 ring consumer for the duration of an active session.
 * Protocol modules live under the webrtc/ subdirectory.
 *
 * See work/reference/WEBRTC_TASK.md and work/reference/PILOT.md M5.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "iotconnect.h"
#include "iotcl.h"

#include "app_webrtc.h"

#include "webrtc/aws_creds.h"


#define APP_WEBRTC_TASK_NAME       ("CM33 WebRTC")
#define APP_WEBRTC_TASK_STACK      (8U * 1024U)
#define APP_WEBRTC_TASK_PRIORITY   (tskIDLE_PRIORITY + 2U)

// Idle poll cadence + the steady-state run_session tick budget. Mirrors
// app_shmem_video; kept identical so future ring-drain coexistence is simple.
#define APP_WEBRTC_POLL_IDLE_MS    (20U)

// Backoff between failed sessions. Flat for now; revisit if churn shows up.
#define APP_WEBRTC_BACKOFF_MS      (1000U)

// Synchronous stop bound. Logs a warning and returns anyway if exceeded.
#define APP_WEBRTC_STOP_TIMEOUT_MS (5000U)


static TaskHandle_t webrtc_task_handle = NULL;
static volatile bool webrtc_running = false;
static volatile bool webrtc_creds_dirty = false;

static AwsCreds aws_creds;


// Pull the M4 triplet + channel ARN from the SDK into our local AwsCreds view.
// Strings remain owned by the SDK (cached IotclDraCredentialsResult and
// IotclMqttConfig). Caller must not call iotconnect_sdk_aws_creds_free()
// while a session is active.
//
// Returns 0 on success, -1 if creds or channel ARN are missing.
//
// Region is left as a const literal for now; will be parsed from the ARN once
// the signaling step lands and AwsCreds is reshaped (PILOT.md M5 §1 step 2).
static int populate_creds(void) {
    const IotclDraCredentialsResult *c = iotconnect_sdk_aws_creds_get();
    if (NULL == c) {
        printf("[webrtc] no AWS creds cached (or expired)\n");
        return -1;
    }

    IotclMqttConfig *mqtt_cfg = iotcl_mqtt_get_config();
    if (NULL == mqtt_cfg || NULL == mqtt_cfg->aws.webrtc_channel_arn) {
        printf("[webrtc] webrtc_channel_arn not available from /IOTCONNECT discovery\n");
        return -1;
    }

    aws_creds.region             = "us-east-1"; // placeholder; parse from ARN at signaling step
    aws_creds.channel_arn        = mqtt_cfg->aws.webrtc_channel_arn;
    aws_creds.access_key_id      = c->access_key_id;
    aws_creds.secret_access_key  = c->secret_access_key;
    aws_creds.session_token      = c->session_token;
    return 0;
}


/* Stub. Real session shape:
 *   - signaling: connect to the KVS channel as viewer, hold the WSS open.
 *   - wait for master-initiated offer over signaling (idle WSS pump).
 *   - on offer: ICE -> DTLS -> SRTP keying -> ring consumer start.
 *   - media flows until master tears down or signaling drops.
 * "Session ended" means signaling fell over, not "browser stopped watching" —
 * a single signaling session may serve many master-initiated viewings.
 * Lands across PILOT.md M5 §1 steps 3..7. For now this proves the lifecycle
 * works: log the populated creds once, return failure, let the loop back off.
 *
 * Hard rule once protocol code lands here: no I/O may block longer than
 * APP_WEBRTC_POLL_IDLE_MS in steady state. See WEBRTC_TASK.md §3.4.
 */
static int run_session(void) {
    int rc = populate_creds();
    if (0 != rc) {
        return rc;
    }

    int seconds_left = iotconnect_sdk_aws_creds_seconds_until_expiry();
    printf("[webrtc] session attempt: ARN=%s region=%s creds_expires_in=%d s\n",
        aws_creds.channel_arn, aws_creds.region, seconds_left
    );
    printf("[webrtc] AKID=%s session_token_len=%d\n",
        aws_creds.access_key_id,
        aws_creds.session_token ? (int) strlen(aws_creds.session_token) : 0
    );

    // Protocol implementation lands here in subsequent sessions.
    return -1;
}


static void webrtc_task(void *arg) {
    (void) arg;

    for (;;) {
        if (!webrtc_running) {
            vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_POLL_IDLE_MS));
            continue;
        }

        // creds_dirty is consulted at session boundary; no behavior wired yet.
        if (webrtc_creds_dirty) {
            webrtc_creds_dirty = false;
            printf("[webrtc] creds_dirty observed at session boundary (no-op)\n");
        }

        int rc = run_session();
        if (0 != rc) {
            printf("[webrtc] session ended rc=%d, backing off %u ms\n",
                rc, (unsigned) APP_WEBRTC_BACKOFF_MS
            );
        }

        // Backoff loop yields promptly on stop request.
        for (uint32_t waited = 0; waited < APP_WEBRTC_BACKOFF_MS && webrtc_running; waited += APP_WEBRTC_POLL_IDLE_MS) {
            vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_POLL_IDLE_MS));
        }
    }
}


void app_webrtc_init(void) {
    if (NULL != webrtc_task_handle) {
        return;
    }
    BaseType_t ok = xTaskCreate(webrtc_task, APP_WEBRTC_TASK_NAME, APP_WEBRTC_TASK_STACK,
        NULL, APP_WEBRTC_TASK_PRIORITY, &webrtc_task_handle
    );
    if (pdPASS != ok) {
        printf("[webrtc] failed to create task\n");
        webrtc_task_handle = NULL;
    }
}


bool app_webrtc_start(void) {
    if (NULL == webrtc_task_handle) {
        printf("[webrtc] start before init\n");
        return false;
    }
    if (webrtc_running) {
        return true;
    }
    webrtc_running = true;
    printf("[webrtc] session loop enabled\n");
    return true;
}


void app_webrtc_stop(void) {
    if (!webrtc_running) {
        return;
    }
    webrtc_running = false;

    // Owner requested sync with 5 s timeout. Real implementation lands when
    // run_session has teardown that takes observable time: replace this with
    // an xTaskNotifyWait keyed off a "task is idle" notify from the loop.
    // Today the loop checks webrtc_running on the next tick and goes idle, so
    // a single tick is sufficient; the timeout is unused.
    (void) APP_WEBRTC_STOP_TIMEOUT_MS;
    vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_POLL_IDLE_MS * 2U));
    printf("[webrtc] stopped\n");
}


void app_webrtc_notify_creds_updated(void) {
    webrtc_creds_dirty = true;
}
