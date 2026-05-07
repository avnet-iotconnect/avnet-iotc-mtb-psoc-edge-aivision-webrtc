/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef WEBRTC_PEER_CONNECTION_H_
#define WEBRTC_PEER_CONNECTION_H_

#include <stddef.h>
#include "webrtc/dtls_transport.h"
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

/* Build an SDP answer body suitable for KVS / Chrome.
 *
 * Inputs:
 *   dt         - DTLS transport handle (D1) for the local SHA-256 fingerprint.
 *                Caller still owns it.
 *   offer      - raw offer SDP (Chrome's), NUL-terminated. Existence is
 *                asserted; full parse is deferred — the answer shape is the
 *                fixed sendonly H.264 mid:0 form Chrome's recvonly viewer
 *                offer expects. Real offer-driven negotiation lands in a
 *                later increment.
 *   offer_len  - convenience; may differ from strlen(offer) if caller has it.
 *   out / cap  - caller-provided output scratch. The serialized SDP is written
 *                into [out, out + *out_len). cap should be >= ~1.5 KB to be
 *                safe; the answer body is typically ~600-900 B.
 *   out_len    - on success, holds the serialized length (no trailing NUL).
 *
 * Returns 0 on success, negative on failure (per-step diagnostics print to
 * UART with the [pc] prefix). */
int peer_connection_build_answer(
    DtlsTransportHandle dt,
    const char *offer,
    size_t offer_len,
    char *out,
    size_t cap,
    size_t *out_len
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
