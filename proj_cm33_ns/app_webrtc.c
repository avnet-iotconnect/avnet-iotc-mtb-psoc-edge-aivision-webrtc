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
#include "webrtc/csprng.h"
#include "webrtc/dtls_transport.h"
#include "webrtc/signaling.h"

// WSS endpoint buffer: "wss://m1.kinesisvideo.<region>.amazonaws.com" — 128 is plenty.
#define APP_WEBRTC_WSS_ENDPOINT_LEN 256

// AWS region codes are bounded at 50 characters by API constraint.
#define APP_WEBRTC_AWS_REGION_MAXLEN 50

// SDP buffer for the offer-side base64 decode. First real Chrome browser
// offer (2026-05-06 hardware run) was 10133 base64 bytes → ~7600 bytes of
// decoded SDP — full WebRTC offers with all H.264 profile-level-id lines,
// RTX, ulpfec, fingerprint, ICE candidates etc. run that big. 12 KB gives
// headroom for the rare extra-bloated offer without re-spending heap pressure
// (PILOT §6).
#define APP_WEBRTC_SDP_BUF_LEN     (12U * 1024U)


#define APP_WEBRTC_TASK_NAME       ("CM33 WebRTC")
#define APP_WEBRTC_TASK_STACK      (8U * 1024U)
// Below app_task (priority 2) for the same reason as app_shmem_video — keep
// the busy WSS/media loop from starving app_task time slices.
#define APP_WEBRTC_TASK_PRIORITY   (tskIDLE_PRIORITY + 1U)

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

// Resolved WSS endpoint, populated synchronously from app_webrtc_start() on the
// caller's thread (today: app_task). The webrtc task only uses the result —
// keeping REST off the webrtc task avoids serializing TLS contexts with the
// SDK's discovery/MQTT flow.
static char webrtc_wss_endpoint[APP_WEBRTC_WSS_ENDPOINT_LEN];



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

    // client_id is unused on the master URL (X-Amz-ClientId is viewer-only)
    // but the AwsCreds field is kept populated so future viewer-side use
    // doesn't have to re-plumb. NULL-tolerant.
    out->region             = region_buf;
    out->channel_arn        = mqtt_cfg->aws.webrtc_channel_arn;
    out->client_id          = mqtt_cfg->client_id;
    out->access_key_id      = c->access_key_id;
    out->secret_access_key  = c->secret_access_key;
    out->session_token      = c->session_token;
    return 0;
}


// Stub for the WSS-side session. Real shape: connect to the signaling channel
// as MASTER, pump WSS for viewer-initiated offers, on offer run ICE -> DTLS ->
// SRTP keying -> ring consumer start, media flows until signaling drops.
// Role naming gotcha: "master" is the on-channel endpoint that browsers
// (KVS "viewers") connect to. A camera *sending* video out is the master.
// "Session ended" means signaling fell over, not "browser stopped watching".
// Hard rule once protocol code lands here: no I/O may block longer than
// APP_WEBRTC_POLL_IDLE_MS in steady state. See WEBRTC_TASK.md §3.4.
//
// Pre-req: app_webrtc_start() resolved webrtc_wss_endpoint synchronously on
// the caller's thread. This task only does WSS+ICE+DTLS+media.
static int run_session(void) {
    char region_buf[APP_WEBRTC_AWS_REGION_MAXLEN + 1];
    AwsCreds aws_creds;
    int rc = populate_creds(&aws_creds, region_buf, sizeof(region_buf));
    if (0 != rc) {
        return rc;
    }

    printf("[webrtc] session attempt: endpoint=%s creds_expires_in=%d s\n",
        webrtc_wss_endpoint, iotconnect_sdk_aws_creds_seconds_until_expiry()
    );

    // Size: WSS endpoint + ARN (URL-encoded ~3x) + session token (URL-encoded ~3x)
    // + fixed overhead: AKID, date, expires, algorithm, signature, signed-headers param.
    size_t signed_url_size = strlen(webrtc_wss_endpoint) + strlen(aws_creds.channel_arn) * 3 + strlen(aws_creds.session_token) * 3 + 512;
    char *signed_url = malloc(signed_url_size);
    if (NULL == signed_url) {
        printf("[webrtc] OOM for signed_url (%d bytes)\n", (int) signed_url_size);
        return -1;
    }
    rc = signaling_build_signed_url(&aws_creds, webrtc_wss_endpoint, signed_url, signed_url_size);
    if (0 != rc) {
        printf("[webrtc] signaling_build_signed_url failed rc=%d\n", rc);
        goto cleanup;
    }
    // First-pass diagnostic: dump the URL so it can be pasted into a JS
    // WebSocket client / wscat to verify the signature is accepted by AWS
    // ahead of the in-tree WS handshake landing.
    printf("[webrtc] presigned master URL: %s\n", signed_url);

    // Zero the local view so the AwsCreds pointers don't sit on the stack for
    // the duration of the WSS session. The SDK-side cached triplet is *not*
    // freed here — failed sessions retry, and re-obtaining via mTLS every
    // back-off would be heavy. Cache lifetime is now bounded by either
    // expiry (M4 plug points) or app_webrtc_stop().
    memset(&aws_creds, 0, sizeof(aws_creds));
    memset(region_buf, 0, sizeof(region_buf));

    // Open the WS connection and keep it alive across the wait_for_offer loop
    // — TLS handoff is paid once at handshake, then wslay drives frame I/O on
    // the same NetworkContext_t. Disconnect happens at session end, not after
    // the upgrade.
    SignalingHandle sig = signaling_connect(signed_url);
    // The presigned URL is multi-KB and signaling_connect has already extracted
    // host+path into its own buffer — free immediately so wslay's ~10 KB of
    // contiguous allocations (frame_ctx ibuf[4096] + event_ctx obuf[4096] +
    // queues) have headroom on a tight heap. See PILOT.md §6 (heap budget).
    free(signed_url);
    signed_url = NULL;
    if (NULL == sig) {
        printf("[webrtc] signaling_connect failed\n");
        return -1;
    }

    printf("[webrtc] WS connection established, waiting for offer...\n");

    char *offer_sdp = malloc(APP_WEBRTC_SDP_BUF_LEN);
    if (NULL == offer_sdp) {
        printf("[webrtc] OOM for SDP buffer (%u bytes)\n", (unsigned) APP_WEBRTC_SDP_BUF_LEN);
        signaling_disconnect(sig);
        return -1;
    }

    // Block until a viewer (browser) publishes an offer or the socket dies.
    // We're MASTER on this channel and may idle indefinitely waiting for a
    // viewer to show up.
    size_t offer_len = 0;
    int wait_rc = signaling_wait_for_offer(sig, offer_sdp, APP_WEBRTC_SDP_BUF_LEN, &offer_len);
    if (0 == wait_rc) {
        printf("[webrtc] WS offer received (%u bytes of SDP)\n", (unsigned) offer_len);

        // Stub SDP_ANSWER for Increment C: prove the envelope round trip.
        // Real SDP arrives with Increment D when DTLS keying is wired and we
        // can fill in fingerprint, ufrag/pwd, and candidate lines. Keep the
        // body short — KVS won't accept it as a working answer either way,
        // and the success criterion here is "browser sees something arrive."
        static const char stub_answer[] =
            "v=0\r\n"
            "o=- 0 0 IN IP4 0.0.0.0\r\n"
            "s=-\r\n"
            "t=0 0\r\n";
        int send_rc = signaling_send_answer(sig, stub_answer);
        if (0 != send_rc) {
            printf("[webrtc] signaling_send_answer failed rc=%d\n", send_rc);
        }
        // Fall through to disconnect either way. Real ICE / DTLS / SRTP / media
        // work continues in Increment D+; for today the round trip is the gate.
    } else {
        printf("[webrtc] signaling_wait_for_offer: error\n");
    }

    free(offer_sdp);
    signaling_disconnect(sig);

    // Always return -1 today — even on the happy round-trip the session has no
    // media path, so run_session must be re-entered after the backoff.
    return -1;

cleanup:
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
        printf("[webrtc] session ended rc=%d — parking task (Increment C diagnostic)\n", rc);
        // TEMP (Increment C bring-up): one session per boot so the log isn't
        // buried under reconnect spam. Restore the back-off-and-retry loop
        // once Increment C is verified.
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }
}


void app_webrtc_init(void) {
    if (NULL != webrtc_task_handle) {
        return;
    }

    printf("[webrtc] init\r\n");

    // Bring the WebRTC-owned DRBG up early. Signaling, ICE, and DTLS will all
    // pull from it; cy-secure-sockets has its own DRBG we can't share.
    if (0 != webrtc_csprng_init()) {
        printf("[webrtc] csprng init failed — abort task creation\n");
        return;
    }

    // TEMP (Increment D1): generate the DTLS cert + fingerprint at boot and
    // print the fingerprint. Verifies cert-gen path before D2 wires it into
    // the SDP answer. Remove these prints once D3 confirms Chrome accepts the
    // answer end-to-end.
    DtlsTransportHandle dt_smoke = dtls_transport_create();
    if (NULL != dt_smoke) {
        char fp[128];
        if (0 == dtls_transport_get_local_fingerprint(dt_smoke, fp, sizeof(fp))) {
            printf("[dtls] local fingerprint: %s\n", fp);
        }
        dtls_transport_destroy(dt_smoke);
    } else {
        printf("[dtls] cert generation smoke test failed\n");
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

    // REST steps run synchronously on the caller's thread (app_task) so the
    // SDK's discovery/MQTT TLS contexts don't race the webrtc task's. The
    // webrtc task only owns long-lived WSS/ICE/DTLS/media work.
    char region_buf[APP_WEBRTC_AWS_REGION_MAXLEN + 1];
    AwsCreds aws_creds;
    if (0 != populate_creds(&aws_creds, region_buf, sizeof(region_buf))) {
        return false;
    }
    if (0 != signaling_resolve_endpoint(&aws_creds, webrtc_wss_endpoint, sizeof(webrtc_wss_endpoint))) {
        printf("[webrtc] signaling_resolve_endpoint failed\n");
        return false;
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
