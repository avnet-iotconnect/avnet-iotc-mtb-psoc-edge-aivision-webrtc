/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

/* M3 stub. Real impl pending: GetSignalingChannelEndpoint via SigV4-signed
 * HTTPS, ConnectAsViewer URL signing, wslay-over-TLS WSS upgrade, JSON
 * envelope decode/encode for SDP exchange. */

#include <stdio.h>
#include <string.h>

#include "webrtc/signaling.h"

struct SignalingCtx {
    int dummy;
};

static struct SignalingCtx g_sig;

int signaling_resolve_endpoint(const AwsCreds *creds, char *out_endpoint, size_t cap) {
    (void) creds;
    printf("signaling_resolve_endpoint: STUB\n");
    if (cap > 0) {
        snprintf(out_endpoint, cap, "wss-XXXX.kinesisvideo.%s.amazonaws.com",
            creds && creds->region ? creds->region : "us-east-1");
    }
    return 0;
}

int signaling_build_signed_viewer_url(
    const AwsCreds *creds,
    const char *wss_endpoint,
    char *out_url,
    size_t cap
) {
    (void) creds;
    printf("signaling_build_signed_viewer_url: STUB (endpoint=%s)\n",
        wss_endpoint ? wss_endpoint : "<null>");
    if (cap > 0) {
        snprintf(out_url, cap, "wss://%s/?role=VIEWER&...sigv4...",
            wss_endpoint ? wss_endpoint : "?");
    }
    return 0;
}

SignalingHandle signaling_connect(const char *signed_url) {
    (void) signed_url;
    printf("signaling_connect: STUB\n");
    return &g_sig;
}

void signaling_disconnect(SignalingHandle sig) {
    (void) sig;
    printf("signaling_disconnect: STUB\n");
}

int signaling_wait_for_offer(SignalingHandle sig, const char **out_offer) {
    (void) sig;
    printf("signaling_wait_for_offer: STUB (returning -1, no real offer)\n");
    if (out_offer) {
        *out_offer = NULL;
    }
    return -1;
}

int signaling_send_answer(SignalingHandle sig, const char *sdp_answer) {
    (void) sig;
    (void) sdp_answer;
    printf("signaling_send_answer: STUB\n");
    return 0;
}
