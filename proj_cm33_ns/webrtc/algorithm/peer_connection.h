/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef WEBRTC_PEER_CONNECTION_H_
#define WEBRTC_PEER_CONNECTION_H_

#include <stddef.h>
#include "dtls_transport.h"
#include "signaling.h"

typedef struct PeerConnectionCtx *PeerConnectionHandle;

PeerConnectionHandle peer_connection_create(void);
void peer_connection_destroy(PeerConnectionHandle pc);

/* Local ICE creds generated for the SDP a=ice-ufrag / a=ice-pwd lines.
 * Surfaced from peer_connection_build_answer so ice_controller can use the
 * same bytes as the STUN message-integrity key (RFC 8445 §7.2.2). Mismatch
 * with the SDP lines would break the browser's connectivity-check signing. */
typedef struct PeerConnectionLocalIceCreds {
    char ufrag[16];        /* NUL-terminated alnum; today PC_ICE_UFRAG_LEN=8. */
    char pwd[32];          /* NUL-terminated alnum; today PC_ICE_PWD_LEN=24.  */
    size_t ufrag_len;
    size_t pwd_len;
} PeerConnectionLocalIceCreds;

/* Remote ICE creds scraped from the offer's a=ice-ufrag / a=ice-pwd lines.
 * Bounds are RFC 8839 §5.4: ufrag is 4-256 chars, pwd is 22-256 chars. We
 * crop conservatively — Chrome ships short values today and we never have to
 * round-trip these. Mismatch fails STUN integrity check at the browser. */
typedef struct PeerConnectionRemoteIceCreds {
    char ufrag[64];
    char pwd[128];
    size_t ufrag_len;
    size_t pwd_len;
} PeerConnectionRemoteIceCreds;

/* Minimal offer parse: scan for a=ice-ufrag:<value> and a=ice-pwd:<value>.
 * Not a full SDP parser — just the two attributes we have to round-trip into
 * the ICE controller. Returns 0 on success (both attrs found and copied). */
int peer_connection_extract_remote_ice_creds(
    const char *offer,
    size_t offer_len,
    PeerConnectionRemoteIceCreds *out
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
 *   local_ip   - device's local IP for o= and c= lines (dotted quad, e.g.
 *                "192.168.38.196").
 *   local_port - UDP port device is bound to (from getsockname). Used in m=
 *                line so browser knows the real port to connect to.
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
    const char *local_ip,
    uint16_t local_port,
    char *out,
    size_t cap,
    size_t *out_len,
    PeerConnectionLocalIceCreds *out_ice_creds
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
