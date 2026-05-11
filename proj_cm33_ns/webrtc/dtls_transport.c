/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * DTLS-SRTP transport for the WebRTC peer connection.
 *
 * Increment D1 (current): self-signed ECDSA P-256 cert + key generation, with
 * the SHA-256 fingerprint string the SDP answer needs ("sha-256 AA:BB:..."). The
 * UDP socket, DTLS handshake, fingerprint verification, and RFC 5705 SRTP key
 * export functions declared in dtls_transport.h are intentionally not defined
 * yet — they land in later increments. The linker only complains if they're
 * called, and nothing calls them today.
 *
 * Why we don't reuse mtb_shared/avnet-iotc-mtb-sdk's iotc_gencert.c despite
 * it being right there: it returns PEM-only and re-seeds its own DRBG per call
 * (we have webrtc_csprng for that). For DTLS-SRTP we want the parsed cert +
 * key in memory anyway (the eventual mbedtls_ssl_conf_own_cert call needs
 * them), and the fingerprint is computed over the DER form. So we go straight
 * to DER, parse it back into mbedtls_x509_crt for handshake reuse, and skip
 * the PEM round trip entirely.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/sha256.h"

/* lwIP BSD sockets for the single shared UDP fd. The project-wide
 * LWIP_TIMEVAL_PRIVATE=0 (wifi-core's lwipopts.h) defers struct timeval to
 * newlib's <sys/time.h>; clangd may not pick that up and squiggle a
 * "redefinition of timeval" — the toolchain is fine. See webrtc/config.h. */
#include "lwip/sockets.h"

#include "webrtc/csprng.h"
#include "webrtc/dtls_transport.h"

// SHA-256 fingerprint formatted as "sha-256 AA:BB:..." is 7 + 32*3 = 103 chars
// including the trailing NUL. Round up for cosmetic headroom.
#define DTLS_FP_BUF_LEN          112U

// DER buffer for the self-signed cert. mbedtls_x509write_crt_der requires its
// scratch buffer big enough for the entire cert + ASN.1 overhead. A P-256
// self-signed cert with a short subject lands around 350 bytes; 1 KB is plenty.
#define DTLS_CERT_DER_BUF_LEN    1024U

// Cert validity window. WebRTC peer certs are ephemeral by design (the
// fingerprint is the only trust anchor) so the dates barely matter — pick a
// long window to dodge clock-skew rejection by paranoid peers. Format is
// "YYYYMMDDHHMMSS" per mbedtls_x509write_crt_set_validity.
#define DTLS_CERT_NOT_BEFORE     "20240101000000"
#define DTLS_CERT_NOT_AFTER      "20440101000000"

// Subject name. WebRTC peers don't validate this field; the fingerprint is what
// matters. Kept short and self-describing for tcpdump/wireshark debug.
#define DTLS_CERT_SUBJECT        "CN=fork-aivision-webrtc"


struct DtlsTransportCtx {
    mbedtls_pk_context key;
    mbedtls_x509_crt cert;
    char fingerprint[DTLS_FP_BUF_LEN];
    /* The single UDP socket multiplexed by ICE / DTLS / RTP via first-byte
     * demux. Opened lazily by dtls_transport_open_socket; -1 until then. */
    int udp_fd;
    uint16_t local_port;
};


// Build "sha-256 AA:BB:CC:..." into out from the 32-byte SHA-256 digest.
// Caller guarantees out has DTLS_FP_BUF_LEN bytes.
static void format_fingerprint_sha256(const unsigned char digest[32], char *out) {
    static const char HEX[] = "0123456789ABCDEF";
    static const char PREFIX[] = "sha-256 ";
    size_t prefix_len = sizeof(PREFIX) - 1;
    memcpy(out, PREFIX, prefix_len);
    char *p = out + prefix_len;
    for (size_t i = 0; i < 32; i++) {
        if (i > 0) {
            *p++ = ':';
        }
        *p++ = HEX[(digest[i] >> 4) & 0x0F];
        *p++ = HEX[digest[i] & 0x0F];
    }
    *p = '\0';
}


// Generate ECDSA P-256 keypair into ctx->key.
static int generate_keypair(struct DtlsTransportCtx *ctx) {
    int rc = mbedtls_pk_setup(&ctx->key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (0 != rc) {
        printf("[dtls] mbedtls_pk_setup failed: -0x%04x\n", (unsigned) -rc);
        return -1;
    }
    rc = mbedtls_ecp_gen_key(
        MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(ctx->key),
        webrtc_csprng_f_rng, webrtc_csprng_p_rng()
    );
    if (0 != rc) {
        printf("[dtls] mbedtls_ecp_gen_key failed: -0x%04x\n", (unsigned) -rc);
        return -1;
    }
    return 0;
}


// Write a self-signed cert for ctx->key into the caller-provided DER scratch.
// On success returns the cert length and sets *out_der to the start of the cert
// inside the scratch buffer (mbedtls_x509write_crt_der writes back-to-front).
static int write_selfsigned_cert_der(
    struct DtlsTransportCtx *ctx,
    unsigned char *der_scratch,
    size_t scratch_len,
    unsigned char **out_der
) {
    mbedtls_x509write_cert wcrt;
    mbedtls_x509write_crt_init(&wcrt);

    // Random 8-byte serial. WebRTC doesn't care about uniqueness across peers,
    // but a random serial avoids the iotc_gencert.c choice of leaking the chip
    // unique-id into the wire cert.
    unsigned char serial[8];
    int rc = webrtc_csprng_bytes(serial, sizeof(serial));
    if (0 != rc) {
        printf("[dtls] csprng bytes for serial failed\n");
        goto exit;
    }

    mbedtls_x509write_crt_set_subject_key(&wcrt, &ctx->key);
    mbedtls_x509write_crt_set_issuer_key(&wcrt, &ctx->key);
    mbedtls_x509write_crt_set_version(&wcrt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&wcrt, MBEDTLS_MD_SHA256);

    rc = mbedtls_x509write_crt_set_subject_name(&wcrt, DTLS_CERT_SUBJECT);
    if (0 != rc) {
        printf("[dtls] set_subject_name failed: -0x%04x\n", (unsigned) -rc);
        goto exit;
    }
    rc = mbedtls_x509write_crt_set_issuer_name(&wcrt, DTLS_CERT_SUBJECT);
    if (0 != rc) {
        printf("[dtls] set_issuer_name failed: -0x%04x\n", (unsigned) -rc);
        goto exit;
    }
    rc = mbedtls_x509write_crt_set_serial_raw(&wcrt, serial, sizeof(serial));
    if (0 != rc) {
        printf("[dtls] set_serial_raw failed: -0x%04x\n", (unsigned) -rc);
        goto exit;
    }
    rc = mbedtls_x509write_crt_set_validity(&wcrt, DTLS_CERT_NOT_BEFORE, DTLS_CERT_NOT_AFTER);
    if (0 != rc) {
        printf("[dtls] set_validity failed: -0x%04x\n", (unsigned) -rc);
        goto exit;
    }
    rc = mbedtls_x509write_crt_set_basic_constraints(&wcrt, 1, 0);
    if (0 != rc) {
        printf("[dtls] set_basic_constraints failed: -0x%04x\n", (unsigned) -rc);
        goto exit;
    }

    // mbedtls_x509write_crt_der writes back-to-front and returns the length.
    rc = mbedtls_x509write_crt_der(
        &wcrt, der_scratch, scratch_len,
        webrtc_csprng_f_rng, webrtc_csprng_p_rng()
    );
    if (rc < 0) {
        printf("[dtls] mbedtls_x509write_crt_der failed: -0x%04x\n", (unsigned) -rc);
        goto exit;
    }
    *out_der = der_scratch + scratch_len - rc;

exit:
    mbedtls_x509write_crt_free(&wcrt);
    return rc;
}


DtlsTransportHandle dtls_transport_create(void) {
    struct DtlsTransportCtx *ctx = calloc(1, sizeof(*ctx));
    if (NULL == ctx) {
        printf("[dtls] OOM for transport ctx\n");
        return NULL;
    }
    mbedtls_pk_init(&ctx->key);
    mbedtls_x509_crt_init(&ctx->cert);
    ctx->udp_fd = -1;
    ctx->local_port = 0;

    if (0 != generate_keypair(ctx)) {
        goto fail;
    }

    unsigned char *der_scratch = malloc(DTLS_CERT_DER_BUF_LEN);
    if (NULL == der_scratch) {
        printf("[dtls] OOM for cert DER scratch\n");
        goto fail;
    }

    unsigned char *cert_der = NULL;
    int cert_len = write_selfsigned_cert_der(ctx, der_scratch, DTLS_CERT_DER_BUF_LEN, &cert_der);
    if (cert_len <= 0) {
        free(der_scratch);
        goto fail;
    }

    // Parse the DER back into ctx->cert so the eventual DTLS handshake can
    // hand it to mbedtls_ssl_conf_own_cert without re-running the writer.
    int rc = mbedtls_x509_crt_parse_der(&ctx->cert, cert_der, (size_t) cert_len);
    if (0 != rc) {
        printf("[dtls] mbedtls_x509_crt_parse_der failed: -0x%04x\n", (unsigned) -rc);
        free(der_scratch);
        goto fail;
    }

    // Compute SHA-256 fingerprint over the DER and stash the formatted string.
    unsigned char digest[32];
    rc = mbedtls_sha256(cert_der, (size_t) cert_len, digest, 0 /* not SHA-224 */);
    free(der_scratch);
    if (0 != rc) {
        printf("[dtls] mbedtls_sha256 failed: -0x%04x\n", (unsigned) -rc);
        goto fail;
    }
    format_fingerprint_sha256(digest, ctx->fingerprint);

    return ctx;

fail:
    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_pk_free(&ctx->key);
    free(ctx);
    return NULL;
}


void dtls_transport_destroy(DtlsTransportHandle dt) {
    if (NULL == dt) {
        return;
    }
    if (dt->udp_fd >= 0) {
        close(dt->udp_fd);
        dt->udp_fd = -1;
    }
    mbedtls_x509_crt_free(&dt->cert);
    mbedtls_pk_free(&dt->key);
    free(dt);
}


int dtls_transport_get_local_fingerprint(DtlsTransportHandle dt, char *out, size_t cap) {
    if (NULL == dt || NULL == out) {
        return -1;
    }
    size_t needed = strlen(dt->fingerprint) + 1;
    if (cap < needed) {
        return -1;
    }
    memcpy(out, dt->fingerprint, needed);
    return 0;
}


int dtls_transport_open_socket(DtlsTransportHandle dt) {
    if (NULL == dt) {
        return -1;
    }
    if (dt->udp_fd >= 0) {
        return 0;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("[dtls] socket(AF_INET, SOCK_DGRAM) failed: errno=%d\n", errno);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = 0;
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        printf("[dtls] bind(INADDR_ANY:0) failed: errno=%d\n", errno);
        close(fd);
        return -1;
    }

    struct sockaddr_in bound;
    socklen_t bound_len = sizeof(bound);
    if (getsockname(fd, (struct sockaddr *) &bound, &bound_len) < 0) {
        printf("[dtls] getsockname failed: errno=%d\n", errno);
        close(fd);
        return -1;
    }

    dt->udp_fd     = fd;
    dt->local_port = ntohs(bound.sin_port);
    printf("[dtls] UDP socket open: fd=%d port=%u\n", fd, (unsigned) dt->local_port);
    return 0;
}


int dtls_transport_get_socket(DtlsTransportHandle dt) {
    if (NULL == dt) {
        return -1;
    }
    return dt->udp_fd;
}


uint16_t dtls_transport_get_local_port(DtlsTransportHandle dt) {
    if (NULL == dt) {
        return 0;
    }
    return dt->local_port;
}
