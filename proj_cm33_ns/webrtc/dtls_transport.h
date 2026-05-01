/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef WEBRTC_DTLS_TRANSPORT_H_
#define WEBRTC_DTLS_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>

/* DTLS-SRTP transport: owns a UDP socket (lwIP BSD), wraps mbedTLS DTLS via
 * BIO callbacks, generates a self-signed cert at start, exposes the local
 * fingerprint for the SDP, verifies the remote fingerprint on handshake,
 * and extracts RFC 5705 keying material for libsrtp.
 *
 * Used by peer_connection.c. ICE controller writes/reads from the same UDP
 * socket via dtls_transport_get_socket() so STUN and DTLS multiplex on one
 * port. */

typedef struct DtlsTransportCtx *DtlsTransportHandle;

DtlsTransportHandle dtls_transport_create(void);
void dtls_transport_destroy(DtlsTransportHandle dt);

/* Local UDP socket fd (lwIP). ICE shares this for STUN reads/writes. */
int dtls_transport_get_socket(DtlsTransportHandle dt);

/* SDP fingerprint string ("sha-256 AA:BB:..."), NUL-terminated. */
int dtls_transport_get_local_fingerprint(DtlsTransportHandle dt, char *out, size_t cap);

/* Drive the handshake. Caller (peer_connection) supplies the remote peer
 * address (selected ICE pair) and the remote fingerprint from the offer. */
int dtls_transport_handshake(
    DtlsTransportHandle dt,
    const struct sockaddr *remote,
    int remote_len,
    const char *expected_fingerprint
);

/* RFC 5705 export for SRTP keying. Caller-supplied buffer must be at least
 * the size required by the negotiated SRTP profile (e.g. 60 bytes for
 * AES_CM_128_HMAC_SHA1_80). */
int dtls_transport_export_srtp_keys(DtlsTransportHandle dt, uint8_t *out, size_t cap);

#endif /* WEBRTC_DTLS_TRANSPORT_H_ */
