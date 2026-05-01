/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

/* WebRTC application flow (M3).
 *
 * Heavy lifting lives in webrtc/ source files. This file orchestrates:
 * creds -> signed URL -> signaling -> peer connection -> ICE -> DTLS -> media flow -> teardown.
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

/* Credentials hardcoded for first-light; paste triplet from a manual IoTC
 * discovery run (plain HTTPS -> mTLS -> 1-hour triplet) and rebuild.
 *
 * Plug point (NOT wired): runtime discovery via iotc-c-lib replaces the
 * populator below; struct signature stays the same.
 * Plug point (NOT wired): cred refresh on 1-hour expiry via timer or
 * on signaling-layer auth-failure. */

#define AWS_REGION            "us-east-1"
#define AWS_CHANNEL_ARN       "arn:aws:kinesisvideo:us-east-1:260030673750:channel/nik-e84-webrtc/1776960005303"
#define AWS_ACCESS_KEY_ID     "REPLACE_ME"
#define AWS_SECRET_ACCESS_KEY "REPLACE_ME"
#define AWS_SESSION_TOKEN     "REPLACE_ME"

static AwsCreds aws_creds;

static void app_webrtc_populate_creds(void) {
    aws_creds.region = AWS_REGION;
    aws_creds.channel_arn = AWS_CHANNEL_ARN;
    aws_creds.access_key_id = AWS_ACCESS_KEY_ID;
    aws_creds.secret_access_key = AWS_SECRET_ACCESS_KEY;
    aws_creds.session_token = AWS_SESSION_TOKEN;
}

static int app_webrtc_run_session(void) {
    int rc;
    char wss_endpoint[256];   // ~256 B stack
    char signed_url[1024];    // ~1 KB stack

    // Endpoint hostname is account+region specific; not derivable from ARN.
    rc = signaling_resolve_endpoint(&aws_creds, wss_endpoint, sizeof(wss_endpoint));
    if (rc != 0) {
        printf("app_webrtc: signaling_resolve_endpoint failed (%d)\n", rc);
        return rc;
    }
    printf("app_webrtc: WSS endpoint resolved: %s\n", wss_endpoint);

    rc = signaling_build_signed_viewer_url(&aws_creds, wss_endpoint, signed_url, sizeof(signed_url));
    if (rc != 0) {
        printf("app_webrtc: signaling_build_signed_viewer_url failed (%d)\n", rc);
        return rc;
    }

    // wslay over secure-sockets TLS; blocks until offer arrives or fails.
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

    // Owns the DTLS handshake and SRTP keying after handshake.
    PeerConnectionHandle pc = peer_connection_create();
    if (pc == NULL) {
        printf("app_webrtc: peer_connection_create failed\n");
        signaling_disconnect(sig);
        return -1;
    }

    // LAN-only no-TURN; shares UDP socket(s) with DTLS; 32-pair table (see PILOT.md M3 RAM budget).
    rc = ice_controller_start(pc, &aws_creds);
    if (rc != 0) {
        printf("app_webrtc: ice_controller_start failed (%d)\n", rc);
        peer_connection_destroy(pc);
        signaling_disconnect(sig);
        return rc;
    }

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

    // Trickle ICE: forwards local candidates to master, accepts remote ones.
    // Drives DTLS handshake once a pair is selected. Blocks until media
    // starts flowing or the session ends.
    rc = peer_connection_run(pc, sig);
    if (rc != 0) {
        printf("app_webrtc: peer_connection_run exited (%d)\n", rc);
    }

    // Started by peer_connection_run once DTLS is up; NALs fed directly from CM55->CM33 ring, no copy.
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

    // One session for first-light; reconnect/retry and cred refresh are future concerns.
    int rc = app_webrtc_run_session();
    printf("app_webrtc: session ended (%d). Idling.\n", rc);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_webrtc_start(void) {
    BaseType_t ok = xTaskCreate(
        app_webrtc_task, "webrtc", APP_WEBRTC_TASK_STACK, NULL,
        APP_WEBRTC_TASK_PRIORITY, NULL
    );
    if (ok != pdPASS) {
        printf("app_webrtc: xTaskCreate failed\n");
    }
}
