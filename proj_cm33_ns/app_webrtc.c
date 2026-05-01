/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

/* WebRTC application flow (M3).
 *
 * Read top-to-bottom. Heavy lifting lives in webrtc/ source files. This file only
 * orchestrates: creds -> signed URL -> signaling -> peer connection ->
 * ICE -> DTLS -> media flow -> teardown.
 *
 * Device acts as VIEWER (slave). Browser is master. See PILOT.md M3.
 */

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app_webrtc.h"

#include "webrtc/aws_creds.h"
#include "webrtc/signaling.h"
#include "webrtc/peer_connection.h"
#include "webrtc/ice_controller.h"
#include "webrtc/media_source_ring.h"

/* ---------------------------------------------------------------------------
 * AWS creds + channel info (M3 Step 6).
 *
 * Hardcoded for first-light. Owner pastes values from a manual run of the
 * IoTC discovery + identity flow (plain HTTPS -> mTLS -> 1-hour triplet)
 * and rebuilds. 1-hour expiry is acceptable for a manual bring-up.
 *
 * Plug point (NOT wired): runtime discovery via iotc-c-lib will replace
 * the populator below. Signature of the AwsCreds struct stays the same so
 * the producer is the only thing that swaps. See PILOT.md M3 Step 6.
 *
 * Plug point (NOT wired): cred refresh on 1-hour expiry. Same struct,
 * called from a timer or on signaling-layer auth-failure. Do not add the
 * call now.
 * --------------------------------------------------------------------------- */

#define AWS_REGION              "us-east-1"
#define AWS_CHANNEL_ARN         "arn:aws:kinesisvideo:us-east-1:000000000000:channel/REPLACE_ME/0000000000000"
#define AWS_ACCESS_KEY_ID       "REPLACE_ME"
#define AWS_SECRET_ACCESS_KEY   "REPLACE_ME"
#define AWS_SESSION_TOKEN       "REPLACE_ME"

static AwsCreds aws_creds;

static void app_webrtc_populate_creds(void) {
    /* Today: copy from #defines above. Tomorrow (out of M3 scope): the
     * iotc-c-lib runtime discovery flow lands its triplet here. */
    aws_creds.region = AWS_REGION;
    aws_creds.channel_arn = AWS_CHANNEL_ARN;
    aws_creds.access_key_id = AWS_ACCESS_KEY_ID;
    aws_creds.secret_access_key = AWS_SECRET_ACCESS_KEY;
    aws_creds.session_token = AWS_SESSION_TOKEN;
}

/* ---------------------------------------------------------------------------
 * Lifecycle. Each phase is a thin step that delegates to a webrtc/ helper.
 * --------------------------------------------------------------------------- */

static int app_webrtc_run_session(void) {
    char signed_url[1024];
    int rc;

    /* 1. Resolve the per-account WSS endpoint (GetSignalingChannelEndpoint).
     *    Endpoint hostname is account+region specific; not derivable from ARN. */
    char wss_endpoint[256];
    rc = signaling_resolve_endpoint(&aws_creds, wss_endpoint, sizeof(wss_endpoint));
    if (rc != 0) {
        printf("app_webrtc: signaling_resolve_endpoint failed (%d)\n", rc);
        return rc;
    }
    printf("app_webrtc: WSS endpoint resolved: %s\n", wss_endpoint);

    /* 2. Sign ConnectAsViewer URL with SigV4. */
    rc = signaling_build_signed_viewer_url(
        &aws_creds, wss_endpoint, signed_url, sizeof(signed_url)
    );
    if (rc != 0) {
        printf("app_webrtc: signaling_build_signed_viewer_url failed (%d)\n", rc);
        return rc;
    }
    printf("app_webrtc: SigV4-signed viewer URL ready\n");

    /* 3. Open signaling (WSS upgrade, wslay over secure-sockets TLS).
     *    Blocks until offer arrives from master, or fails. */
    SignalingHandle sig = signaling_connect(signed_url);
    if (sig == NULL) {
        printf("app_webrtc: signaling_connect failed\n");
        return -1;
    }
    printf("app_webrtc: signaling connected, waiting for SDP offer\n");

    const char *sdp_offer = NULL;
    rc = signaling_wait_for_offer(sig, &sdp_offer);
    if (rc != 0) {
        printf("app_webrtc: signaling_wait_for_offer failed (%d)\n", rc);
        signaling_disconnect(sig);
        return rc;
    }
    printf("app_webrtc: SDP offer received\n");

    /* 4. Bring up the peer connection (RTP/RTCP, libsrtp, dtls_transport).
     *    Owns the DTLS handshake and SRTP keying after handshake. */
    PeerConnectionHandle pc = peer_connection_create();
    if (pc == NULL) {
        printf("app_webrtc: peer_connection_create failed\n");
        signaling_disconnect(sig);
        return -1;
    }

    /* 5. ICE candidate gather + pairing. Shares the UDP socket(s) with DTLS;
     *    LAN-only no-TURN, 32-pair table (see PILOT.md M3 RAM budget). */
    rc = ice_controller_start(pc, &aws_creds);
    if (rc != 0) {
        printf("app_webrtc: ice_controller_start failed (%d)\n", rc);
        peer_connection_destroy(pc);
        signaling_disconnect(sig);
        return rc;
    }

    /* 6. Apply the offer, build + send the answer over signaling. */
    char sdp_answer[4096];
    rc = peer_connection_apply_offer(pc, sdp_offer, sdp_answer, sizeof(sdp_answer));
    if (rc != 0) {
        printf("app_webrtc: peer_connection_apply_offer failed (%d)\n", rc);
        goto session_teardown;
    }
    rc = signaling_send_answer(sig, sdp_answer);
    if (rc != 0) {
        printf("app_webrtc: signaling_send_answer failed (%d)\n", rc);
        goto session_teardown;
    }
    printf("app_webrtc: SDP answer sent\n");

    /* 7. Trickle ICE: forward local candidates to master, accept remote ones.
     *    Drives DTLS handshake once a pair is selected. Blocks until media
     *    starts flowing or the session ends. */
    rc = peer_connection_run(pc, sig);
    if (rc != 0) {
        printf("app_webrtc: peer_connection_run exited (%d)\n", rc);
    }

    /* 8. Media source: hand encoded NALs from the CM55->CM33 ring directly
     *    to PeerConnection_WriteFrame(). No copy. Started by peer_connection_run
     *    once DTLS is up; stopped here on the way out. */
    media_source_ring_stop();

session_teardown:
    ice_controller_stop(pc);
    peer_connection_destroy(pc);
    signaling_disconnect(sig);
    return rc;
}

static void app_webrtc_task(void *pv) {
    (void) pv;

    printf("app_webrtc: task starting\n");

    app_webrtc_populate_creds();

    /* For first-light: run one session, then idle. Reconnect/retry policy is
     * a future concern (also where cred refresh would slot in). */
    int rc = app_webrtc_run_session();
    printf("app_webrtc: session ended (%d). Idling.\n", rc);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_webrtc_start(void) {
    BaseType_t ok = xTaskCreate(
        app_webrtc_task,
        "webrtc",
        APP_WEBRTC_TASK_STACK / sizeof(StackType_t),
        NULL,
        APP_WEBRTC_TASK_PRIORITY,
        NULL
    );
    if (ok != pdPASS) {
        printf("app_webrtc: xTaskCreate failed\n");
    }
}
