/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

/* M3 stub. Real impl pending: SDP munge via awslabs-sdp, RTP/RTCP packetize
 * via awslabs-rtp/rtcp, libsrtp keying + protect, dtls_transport handshake,
 * trickle-ICE candidate exchange via signaling, RTCP PLI/FIR -> IDR-on-demand. */

#include <stdio.h>
#include <string.h>

#include "webrtc/peer_connection.h"

struct PeerConnectionCtx {
    int dummy;
};

static struct PeerConnectionCtx g_pc;

PeerConnectionHandle peer_connection_create(void) {
    printf("peer_connection_create: STUB\n");
    return &g_pc;
}

void peer_connection_destroy(PeerConnectionHandle pc) {
    (void) pc;
    printf("peer_connection_destroy: STUB\n");
}

int peer_connection_apply_offer(
    PeerConnectionHandle pc,
    const char *sdp_offer,
    char *out_sdp_answer,
    size_t cap
) {
    (void) pc;
    (void) sdp_offer;
    printf("peer_connection_apply_offer: STUB\n");
    if (cap > 0 && out_sdp_answer) {
        snprintf(out_sdp_answer, cap, "v=0\r\n... STUB SDP ANSWER ...\r\n");
    }
    return 0;
}

int peer_connection_run(PeerConnectionHandle pc, SignalingHandle sig) {
    (void) pc;
    (void) sig;
    printf("peer_connection_run: STUB (no real session)\n");
    return 0;
}

int PeerConnection_WriteFrame(
    PeerConnectionHandle pc,
    const unsigned char *nal,
    size_t nal_len,
    unsigned long pts_ms,
    int is_idr
) {
    (void) pc;
    (void) nal;
    (void) pts_ms;
    /* Avoid newlib-nano %z hazard: cast size_t to int. */
    printf("PeerConnection_WriteFrame: STUB len=%d idr=%d\n", (int) nal_len, is_idr);
    return 0;
}
