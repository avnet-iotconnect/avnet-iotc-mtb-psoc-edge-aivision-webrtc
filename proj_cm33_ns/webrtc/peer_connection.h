/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef WEBRTC_PEER_CONNECTION_H_
#define WEBRTC_PEER_CONNECTION_H_

#include <stddef.h>
#include "webrtc/signaling.h"

typedef struct PeerConnectionCtx *PeerConnectionHandle;

PeerConnectionHandle peer_connection_create(void);
void peer_connection_destroy(PeerConnectionHandle pc);

/* Apply the remote offer; produce our local answer. */
int peer_connection_apply_offer(
    PeerConnectionHandle pc,
    const char *sdp_offer,
    char *out_sdp_answer,
    size_t cap
);

/* Drives the session after answer is sent: ICE pairing, DTLS handshake,
 * SRTP keying, RTP send loop (calls into media_source_ring), RTCP receive
 * loop (PLI/FIR -> IDR-on-demand seq bump on shared mem). Blocks until
 * disconnect.
 *
 * Holds a reference to sig for trickle-ICE candidate exchange. */
int peer_connection_run(PeerConnectionHandle pc, SignalingHandle sig);

/* RTP write entry called by media_source_ring per NAL.
 * Pointer must remain valid until this call returns (it does - the ring
 * keeps the slot reserved across the call). */
int PeerConnection_WriteFrame(
    PeerConnectionHandle pc,
    const unsigned char *nal,
    size_t nal_len,
    unsigned long pts_ms,
    int is_idr
);

#endif /* WEBRTC_PEER_CONNECTION_H_ */
