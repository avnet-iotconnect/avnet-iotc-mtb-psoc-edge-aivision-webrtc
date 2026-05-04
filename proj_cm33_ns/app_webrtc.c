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
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "iotconnect.h"
#include "iotcl.h"

#include "app_webrtc.h"

#include "webrtc/aws_creds.h"
#include "webrtc/signaling.h"

// WSS endpoint buffer: "wss://m1.kinesisvideo.<region>.amazonaws.com" — 128 is plenty.
#define APP_WEBRTC_WSS_ENDPOINT_LEN 256

// AWS region codes are bounded at 50 characters by API constraint.
#define APP_WEBRTC_AWS_REGION_MAXLEN 50


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



// Pull the M4 triplet + channel ARN from the SDK into our local AwsCreds view.
// Strings remain owned by the SDK (cached IotclDraCredentialsResult and
// IotclMqttConfig). Caller must not call iotconnect_sdk_aws_creds_free()
// while a session is active.
//
// Returns 0 on success, -1 if creds or channel ARN are missing.
static int populate_creds(AwsCreds *out, char *region_buf, size_t region_buf_size) {
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

    // Parse region from ARN: "arn:aws:kinesisvideo:<region>:<account>:..."
    // The region is the 4th colon-separated field (index 3).
    const char *arn = mqtt_cfg->aws.webrtc_channel_arn;
    int colons = 0;
    const char *region_start = NULL;
    for (const char *p = arn; *p; p++) {
        if (':' == *p) {
            colons++;
            if (3 == colons) {
                region_start = p + 1;
            } else if (4 == colons && NULL != region_start) {
                size_t region_len = (size_t)(p - region_start);
                if (region_len == 0 || region_len >= region_buf_size) {
                    printf("[webrtc] region in ARN is invalid or too long\n");
                    return -1;
                }
                memcpy(region_buf, region_start, region_len);
                region_buf[region_len] = '\0';
                break;
            }
        }
    }
    if (NULL == region_start || '\0' == region_buf[0]) {
        printf("[webrtc] failed to parse region from ARN: %s\n", arn);
        return -1;
    }

    out->region             = region_buf;
    out->channel_arn        = mqtt_cfg->aws.webrtc_channel_arn;
    out->access_key_id      = c->access_key_id;
    out->secret_access_key  = c->secret_access_key;
    out->session_token      = c->session_token;
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
    char region_buf[APP_WEBRTC_AWS_REGION_MAXLEN + 1];
    AwsCreds aws_creds;
    int rc = populate_creds(&aws_creds, region_buf, sizeof(region_buf));
    if (0 != rc) {
        return rc;
    }

    int seconds_left = iotconnect_sdk_aws_creds_seconds_until_expiry();
    printf("[webrtc] session attempt: ARN=%s region=%s creds_expires_in=%d s\n",
        aws_creds.channel_arn, aws_creds.region, seconds_left
    );

    char *wss_endpoint = NULL;
    char *signed_url = NULL;
    size_t signed_url_size = 0;

    // Step 5a: resolve the WSS endpoint via GetSignalingChannelEndpoint.
    wss_endpoint = malloc(APP_WEBRTC_WSS_ENDPOINT_LEN);
    if (NULL == wss_endpoint) {
        printf("[webrtc] OOM for wss_endpoint\n");
        return -1;
    }
    rc = signaling_resolve_endpoint(&aws_creds, wss_endpoint, APP_WEBRTC_WSS_ENDPOINT_LEN);
    if (0 != rc) {
        printf("[webrtc] signaling_resolve_endpoint failed rc=%d\n", rc);
        goto cleanup;
    }

    // Step 5b+ stubs: build signed URL, connect, wait for offer. Not yet implemented.
    // Size: WSS endpoint + ARN (URL-encoded ~3x) + session token (URL-encoded ~3x)
    // + fixed overhead: AKID, date, expires, algorithm, signature, signed-headers param.
    signed_url_size = strlen(wss_endpoint)
        + strlen(aws_creds.channel_arn) * 3
        + (aws_creds.session_token ? strlen(aws_creds.session_token) * 3 : 0)
        + 512;
    signed_url = malloc(signed_url_size);
    if (NULL == signed_url) {
        printf("[webrtc] OOM for signed_url (%d bytes)\n", (int) signed_url_size);
        rc = -1;
        goto cleanup;
    }
    rc = signaling_build_signed_viewer_url(&aws_creds, wss_endpoint, signed_url, signed_url_size);
    if (0 != rc) {
        printf("[webrtc] signaling_build_signed_viewer_url not yet implemented\n");
        goto cleanup;
    }

    // Signing is done — raw creds are no longer needed. Drop the SDK heap copy
    // and zero the local view so sensitive key material doesn't sit in RAM
    // for the duration of the WSS session (which may be hours).
    iotconnect_sdk_aws_creds_free();
    memset(&aws_creds, 0, sizeof(aws_creds));
    memset(region_buf, 0, sizeof(region_buf));

    // Protocol implementation continues in subsequent sessions.
    rc = -1;

cleanup:
    free(wss_endpoint);
    free(signed_url);
    return rc;
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
