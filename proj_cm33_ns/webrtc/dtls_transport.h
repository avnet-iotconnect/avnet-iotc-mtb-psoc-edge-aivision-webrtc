/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef WEBRTC_DTLS_TRANSPORT_H_
#define WEBRTC_DTLS_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>

/* Forward-declared so callers that don't already have lwIP/POSIX sockets
 * visible can still include this header. dtls_transport.c (and any caller
 * that actually constructs the sockaddr) pulls "lwip/sockets.h" itself. */
struct sockaddr;

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

/* Local UDP socket fd (lwIP). ICE shares this for STUN reads/writes.
 * The socket is opened lazily by dtls_transport_open_socket(); calling this
 * before that returns -1. */
int dtls_transport_get_socket(DtlsTransportHandle dt);

/* Local UDP port (host byte order) the socket is bound to. The kernel picks
 * the port at bind time (we bind to INADDR_ANY:0). Returns 0 if the socket
 * isn't open yet. */
uint16_t dtls_transport_get_local_port(DtlsTransportHandle dt);

/* Open the single UDP socket (lwIP BSD, AF_INET / SOCK_DGRAM, bind to
 * INADDR_ANY:0). Discovers the kernel-picked port via getsockname so
 * dtls_transport_get_local_port() can return it. Returns 0 on success.
 * Idempotent: a second call with the socket already open is a no-op success. */
int dtls_transport_open_socket(DtlsTransportHandle dt);

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
