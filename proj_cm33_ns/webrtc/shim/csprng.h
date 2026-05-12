/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef WEBRTC_CSPRNG_H_
#define WEBRTC_CSPRNG_H_

#include <stddef.h>
#include <stdint.h>

/* CSPRNG / DRBG owned by the WebRTC subsystem.
 *
 * mbedTLS CTR_DRBG seeded from MBEDTLS_ENTROPY_HARDWARE_ALT (Infineon TRNG via
 * mbedtls_hardware_poll). The TLS/MQTT stacks under cy-secure-sockets keep
 * their own DRBG context — we cannot reach in. So WebRTC needs its own,
 * shared across signaling (Sec-WebSocket-Key), ICE (ufrag/pwd), and DTLS
 * (cert serial / ECDSA key).
 *
 * The DRBG context itself is internal. For mbedTLS APIs that take an
 * f_rng/p_rng pair (cert generation, ECDSA, DTLS handshake), use
 * webrtc_csprng_f_rng + webrtc_csprng_p_rng().
 *
 * Initialization is idempotent — call sites that need RNG before first use
 * should call webrtc_csprng_init() defensively. Returns 0 on success;
 * negative on entropy / DRBG seed failure.
 *
 * Thread safety: the DRBG is wrapped in a FreeRTOS mutex internally so
 * concurrent callers from the webrtc task and any future helper task
 * (e.g. ICE keepalive) can share it. */

int webrtc_csprng_init(void);

/* Fill out_buf with cryptographically strong random bytes.
 * Returns 0 on success, negative on failure. The DRBG must be initialized;
 * if it is not, this function returns an error rather than implicitly seeding
 * (so callers don't accidentally pay the entropy cost on a hot path). */
int webrtc_csprng_bytes(uint8_t *out_buf, size_t len);

/* mbedTLS-compatible callback. Pass these as (f_rng, p_rng) to mbedTLS APIs
 * such as mbedtls_x509write_crt_set_serial_raw, mbedtls_pk_ecdsa_genkey, etc.
 * webrtc_csprng_p_rng() returns the internal DRBG context pointer. */
int webrtc_csprng_f_rng(void *p_rng, unsigned char *out, size_t len);
void *webrtc_csprng_p_rng(void);

#endif /* WEBRTC_CSPRNG_H_ */
