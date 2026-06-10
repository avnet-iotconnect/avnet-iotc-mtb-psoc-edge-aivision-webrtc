/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * Handshake driver, BIO glue, retransmit timer, and SRTP keying-material
 * extraction are adapted from awslabs/freertos-webrtc-reference (Apache-2.0)
 * and ported to mbedTLS 3.6.x.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#if defined(MBEDTLS_PSA_CRYPTO_C)
#include "psa/crypto.h"
#endif

#include "csprng.h"
#include "transport_dtls_mbedtls.h"

#define DTLS_FINGERPRINT_SHA256_BYTES 32U
#define DTLS_FINGERPRINT_STRING_LEN ((DTLS_FINGERPRINT_SHA256_BYTES * 3U) - 1U)
#define DTLS_CERT_SERIAL_LEN 16U

typedef struct DtlsCompatWriteContext {
    mbedtls_x509write_cert cert;
} DtlsCompatWriteContext_t;


/* DTLS-SRTP profiles we advertise in the use_srtp extension (RFC 5764). */
static const mbedtls_ssl_srtp_profile DTLS_SRTP_SUPPORTED_PROFILES[] = {
    MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
    MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32,
    MBEDTLS_TLS_SRTP_UNSET,
};


static int format_validity_time(time_t when, char out[16]) {
    struct tm t;

    if (NULL == gmtime_r(&when, &t)) {
        return -1;
    }
    if (0U == strftime(out, 16U, "%Y%m%d%H%M%S", &t)) {
        return -1;
    }

    return 0;
}


static int bytes_to_fingerprint_string(const uint8_t *bytes, size_t bytes_len, char *out, size_t out_len) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t needed = (0U == bytes_len) ? 1U : ((bytes_len * 3U) - 1U + 1U);
    size_t out_i = 0U;

    if (NULL == bytes || NULL == out || out_len < needed) {
        return -1;
    }

    for (size_t i = 0; i < bytes_len; i++) {
        if (0U != i) {
            out[out_i++] = ':';
        }
        out[out_i++] = HEX[(bytes[i] >> 4) & 0x0FU];
        out[out_i++] = HEX[bytes[i] & 0x0FU];
    }
    out[out_i] = '\0';

    return 0;
}


static void cleanup_write_context(DtlsCompatWriteContext_t *ctx) {
    mbedtls_x509write_crt_free(&ctx->cert);
}


/* ---- FreeRTOS-backed DTLS retransmit timer.
 *
 * mbedTLS's TIMING_C is off in our config, so we supply the timer pair via
 * mbedtls_ssl_set_timer_cb. The contract per mbedTLS docs:
 *   set_cb(ctx, int_ms, fin_ms):
 *     - fin_ms == 0   -> cancel
 *     - else          -> arm; "intermediate" at int_ms, "final" at fin_ms
 *   get_cb(ctx) returns:
 *     -1 cancelled, 0 nothing elapsed, 1 only intermediate elapsed, 2 final.
 */

static void dtls_timer_set(void *ctx, uint32_t int_ms, uint32_t fin_ms) {
    DtlsSessionTimer_t *t = (DtlsSessionTimer_t *) ctx;

    t->int_ms = int_ms;
    t->fin_ms = fin_ms;
    t->start_ticks = (uint32_t) xTaskGetTickCount();
}


static int dtls_timer_get(void *ctx) {
    DtlsSessionTimer_t *t = (DtlsSessionTimer_t *) ctx;
    uint32_t elapsed_ms;

    if (0U == t->fin_ms) {
        return -1;
    }

    elapsed_ms = (uint32_t) ((xTaskGetTickCount() - (TickType_t) t->start_ticks) * portTICK_PERIOD_MS);

    if (elapsed_ms >= t->fin_ms) {
        return 2;
    }
    if (elapsed_ms >= t->int_ms) {
        return 1;
    }
    return 0;
}


/* ---- BIO wrappers.
 *
 * Outbound: hand the buffer to the registered OnTransportDtlsSendHook_t, which
 * routes it through IceController to the remote peer over UDP.
 * Inbound: read from a per-session queued packet that DTLS_ProcessPacket
 * publishes before calling into mbedtls_ssl_read.
 */

static int dtls_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    DtlsTransportParams_t *p = (DtlsTransportParams_t *) ctx;

    if (NULL == p || NULL == p->onDtlsSendHook) {
        return -1;
    }
    return (int) p->onDtlsSendHook(p->pOnDtlsSendCustomContext, buf, len);
}


static int dtls_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    DtlsTransportParams_t *p = (DtlsTransportParams_t *) ctx;
    size_t remaining;
    int to_copy;

    if (NULL == p || NULL == buf) {
        return -1;
    }
    if (NULL == p->pReceivedPacket) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    remaining = p->receivedPacketLength - p->receivedPacketOffset;
    to_copy = (len < remaining) ? (int) len : (int) remaining;
    memcpy(buf, p->pReceivedPacket + p->receivedPacketOffset, (size_t) to_copy);

    p->receivedPacketOffset += (uint32_t) to_copy;
    if (p->receivedPacketOffset >= p->receivedPacketLength) {
        p->pReceivedPacket = NULL;
        p->receivedPacketLength = 0U;
        p->receivedPacketOffset = 0U;
    }
    return to_copy;
}


/* ---- Export-keys callback (mbedTLS 3.x).
 *
 * Called by mbedTLS at the end of the handshake. We stash the master secret
 * and randoms so DTLS_PopulateKeyingMaterial can later derive SRTP keys with
 * the "EXTRACTOR-dtls_srtp" PRF label (RFC 5705 / RFC 5764).
 */
static void dtls_export_keys_cb(void *ctx,
                                mbedtls_ssl_key_export_type type,
                                const unsigned char *secret,
                                size_t secret_len,
                                const unsigned char client_random[32],
                                const unsigned char server_random[32],
                                mbedtls_tls_prf_types prf_type) {
    DtlsSSLContext_t *ssl_ctx = (DtlsSSLContext_t *) ctx;
    TlsKeys *keys;
    size_t to_copy;

    if (NULL == ssl_ctx) {
        return;
    }
    if (MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET != type) {
        return;
    }

    keys = &ssl_ctx->tlsKeys;
    to_copy = (secret_len < sizeof(keys->masterSecret)) ? secret_len : sizeof(keys->masterSecret);

    memcpy(keys->masterSecret, secret, to_copy);
    memcpy(keys->randBytes, client_random, MAX_DTLS_RANDOM_BYTES_LEN);
    memcpy(keys->randBytes + MAX_DTLS_RANDOM_BYTES_LEN, server_random, MAX_DTLS_RANDOM_BYTES_LEN);
    keys->tlsProfile = prf_type;
}


static DtlsTransportStatus_t configure_ssl(DtlsSSLContext_t *ssl_ctx, DtlsNetworkCredentials_t *creds, uint8_t is_server) {
    int rc;

    ssl_ctx->certProfile = mbedtls_x509_crt_profile_default;

    rc = mbedtls_ssl_config_defaults(&ssl_ctx->config,
                                     is_server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (0 != rc) {
        LogError(("mbedtls_ssl_config_defaults failed: -0x%04x", (unsigned int) -rc));
        return DTLS_TRANSPORT_INSUFFICIENT_MEMORY;
    }

    /* WebRTC peers use self-signed certs; trust is by SDP fingerprint, not chain. */
    mbedtls_ssl_conf_authmode(&ssl_ctx->config, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&ssl_ctx->config, mbedtls_ctr_drbg_random, &ssl_ctx->ctrDrbgContext);
    mbedtls_ssl_conf_cert_profile(&ssl_ctx->config, &ssl_ctx->certProfile);

    if (NULL == creds->pClientCert || NULL == creds->pPrivateKey) {
        LogError(("DTLS credentials missing: cert=%p key=%p", creds->pClientCert, creds->pPrivateKey));
        return DTLS_TRANSPORT_INVALID_CREDENTIALS;
    }

    rc = mbedtls_ssl_conf_own_cert(&ssl_ctx->config, creds->pClientCert, creds->pPrivateKey);
    if (0 != rc) {
        LogError(("mbedtls_ssl_conf_own_cert failed: -0x%04x", (unsigned int) -rc));
        return DTLS_TRANSPORT_INVALID_CREDENTIALS;
    }

    rc = mbedtls_ssl_conf_dtls_srtp_protection_profiles(&ssl_ctx->config,
                                                       DTLS_SRTP_SUPPORTED_PROFILES);
    if (0 != rc) {
        LogError(("mbedtls_ssl_conf_dtls_srtp_protection_profiles failed: -0x%04x", (unsigned int) -rc));
        return DTLS_TRANSPORT_INTERNAL_ERROR;
    }

    rc = mbedtls_ssl_setup(&ssl_ctx->context, &ssl_ctx->config);
    if (0 != rc) {
        LogError(("mbedtls_ssl_setup failed: -0x%04x", (unsigned int) -rc));
        return DTLS_TRANSPORT_INTERNAL_ERROR;
    }

    /* Per-context: export-keys callback (mbedTLS 3.x API). */
    mbedtls_ssl_set_export_keys_cb(&ssl_ctx->context, dtls_export_keys_cb, ssl_ctx);

    return DTLS_SUCCESS;
}


static DtlsTransportStatus_t seed_drbg(mbedtls_entropy_context *entropy, mbedtls_ctr_drbg_context *drbg) {
    int rc;

#if defined(MBEDTLS_PSA_CRYPTO_C)
    psa_status_t pstat = psa_crypto_init();

    /* psa_crypto_init is idempotent; PSA_SUCCESS or PSA_ERROR_ALREADY_EXISTS are both fine. */
    if (PSA_SUCCESS != pstat && PSA_ERROR_ALREADY_EXISTS != pstat) {
        LogError(("psa_crypto_init failed: %d", (int) pstat));
        return DTLS_TRANSPORT_INTERNAL_ERROR;
    }
#endif

    rc = mbedtls_ctr_drbg_seed(drbg, mbedtls_entropy_func, entropy, NULL, 0);
    if (0 != rc) {
        LogError(("mbedtls_ctr_drbg_seed failed: -0x%04x", (unsigned int) -rc));
        return DTLS_TRANSPORT_INTERNAL_ERROR;
    }
    return DTLS_SUCCESS;
}


DtlsTransportStatus_t DTLS_Init(
    DtlsNetworkContext_t *pNetworkContext,
    DtlsNetworkCredentials_t *pNetworkCredentials,
    uint8_t isServer
) {
    DtlsTransportParams_t *params;
    DtlsSSLContext_t *ssl_ctx;
    DtlsTransportStatus_t status;

    if (NULL == pNetworkContext || NULL == pNetworkContext->pParams || NULL == pNetworkCredentials) {
        return DTLS_INVALID_PARAMETER;
    }
    if (0 != webrtc_csprng_init()) {
        return DTLS_GENERATE_RANDOM_BITS_FAILURE;
    }

    params = pNetworkContext->pParams;
    ssl_ctx = &params->dtlsSslContext;

    /* Zero-init all mbedTLS contexts so the free path is always safe. */
    mbedtls_ssl_init(&ssl_ctx->context);
    mbedtls_ssl_config_init(&ssl_ctx->config);
    mbedtls_x509_crt_init(&ssl_ctx->rootCa);
    mbedtls_x509_crt_init(&ssl_ctx->clientCert);
    mbedtls_pk_init(&ssl_ctx->privKey);
    mbedtls_entropy_init(&ssl_ctx->entropyContext);
    mbedtls_ctr_drbg_init(&ssl_ctx->ctrDrbgContext);
    memset(&ssl_ctx->tlsKeys, 0, sizeof(ssl_ctx->tlsKeys));
    memset(&params->mbedtlsTimer, 0, sizeof(params->mbedtlsTimer));
    pNetworkContext->state = DTLS_STATE_NEW;

    status = seed_drbg(&ssl_ctx->entropyContext, &ssl_ctx->ctrDrbgContext);
    if (DTLS_SUCCESS != status) {
        return status;
    }

    status = configure_ssl(ssl_ctx, pNetworkCredentials, isServer);
    if (DTLS_SUCCESS != status) {
        return status;
    }

    mbedtls_ssl_set_timer_cb(&ssl_ctx->context, &params->mbedtlsTimer, dtls_timer_set, dtls_timer_get);
    mbedtls_ssl_set_bio(&ssl_ctx->context, params, dtls_bio_send, dtls_bio_recv, NULL);

    pNetworkContext->state = DTLS_STATE_HANDSHAKING;
    LogInfo(("DTLS context %p initialized (role=%s)", pNetworkContext, isServer ? "server" : "client"));
    return DTLS_SUCCESS;
}


void DTLS_Disconnect(DtlsNetworkContext_t *pNetworkContext) {
    DtlsSSLContext_t *ssl_ctx;

    if (NULL == pNetworkContext || NULL == pNetworkContext->pParams) {
        return;
    }

    ssl_ctx = &pNetworkContext->pParams->dtlsSslContext;
    (void) mbedtls_ssl_close_notify(&ssl_ctx->context);

    mbedtls_ssl_free(&ssl_ctx->context);
    mbedtls_ssl_config_free(&ssl_ctx->config);
    mbedtls_x509_crt_free(&ssl_ctx->rootCa);
    mbedtls_x509_crt_free(&ssl_ctx->clientCert);
    mbedtls_pk_free(&ssl_ctx->privKey);
    mbedtls_entropy_free(&ssl_ctx->entropyContext);
    mbedtls_ctr_drbg_free(&ssl_ctx->ctrDrbgContext);
    pNetworkContext->state = DTLS_STATE_NONE;
}


int32_t DTLS_Send(DtlsNetworkContext_t *pNetworkContext, const void *pBuffer, size_t bytesToSend) {
    int32_t ret;

    if (
        NULL == pNetworkContext ||
        NULL == pNetworkContext->pParams ||
        NULL == pBuffer ||
        0U == bytesToSend
    ) {
        return -1;
    }

    ret = (int32_t) mbedtls_ssl_write(&pNetworkContext->pParams->dtlsSslContext.context, pBuffer, bytesToSend);

    /* Caller may retry on WANT_READ/WANT_WRITE/TIMEOUT — surface as 0 like the reference. */
    if (MBEDTLS_ERR_SSL_WANT_READ == ret || MBEDTLS_ERR_SSL_WANT_WRITE == ret || MBEDTLS_ERR_SSL_TIMEOUT == ret) {
        ret = 0;
    }
    return ret;
}


int32_t DTLS_GetSocketFd(DtlsNetworkContext_t *pNetworkContext) {
    (void) pNetworkContext;
    return -1;
}


DtlsTransportStatus_t DTLS_ProcessPacket(
    DtlsNetworkContext_t *pNetworkContext,
    void *pDtlsPacket,
    size_t dtlsPacketLength,
    uint8_t *readBuffer,
    size_t *pReadBufferSize
) {
    DtlsTransportParams_t *params;
    int rc = MBEDTLS_ERR_SSL_WANT_READ;
    int read_offset = 0;

    if (
        NULL == pNetworkContext ||
        NULL == pNetworkContext->pParams ||
        NULL == pDtlsPacket ||
        NULL == readBuffer ||
        NULL == pReadBufferSize
    ) {
        return DTLS_INVALID_PARAMETER;
    }

    params = pNetworkContext->pParams;

    /* Publish the inbound packet for dtls_bio_recv to drain. */
    params->pReceivedPacket = (uint8_t *) pDtlsPacket;
    params->receivedPacketLength = dtlsPacketLength;
    params->receivedPacketOffset = 0U;

    /* mbedtls_ssl_read will internally drive the handshake until it's complete,
     * then start delivering app data. Loop while we still have bytes queued
     * and mbedTLS keeps asking for more. */
    while (MBEDTLS_ERR_SSL_WANT_READ == rc && NULL != params->pReceivedPacket) {
        rc = mbedtls_ssl_read(&params->dtlsSslContext.context,
                              readBuffer + read_offset,
                              *pReadBufferSize - (size_t) read_offset);

        if (MBEDTLS_ERR_SSL_WANT_READ == rc || MBEDTLS_ERR_SSL_WANT_WRITE == rc || MBEDTLS_ERR_SSL_TIMEOUT == rc) {
            /* Need more input from the network, or output already queued. */
        } else if (MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY == rc) {
            LogInfo(("DTLS connection closed by peer."));
            *pReadBufferSize = (size_t) read_offset;
            return DTLS_CONNECTION_HAS_BEEN_CLOSED;
        } else if (rc < 0) {
            LogError(("mbedtls_ssl_read failed: -0x%04x", (unsigned int) -rc));
            *pReadBufferSize = (size_t) read_offset;
            return DTLS_TRANSPORT_PROCESS_FAILURE;
        } else {
            read_offset += rc;
        }
    }

    *pReadBufferSize = (size_t) read_offset;

    /* If the handshake just completed, surface that to the caller so they can
     * pull keying material and validate the remote fingerprint. */
    if (DTLS_STATE_HANDSHAKING == pNetworkContext->state &&
        0 != mbedtls_ssl_is_handshake_over(&params->dtlsSslContext.context)) {
        pNetworkContext->state = DTLS_STATE_READY;
        return DTLS_HANDSHAKE_COMPLETE;
    }

    return DTLS_SUCCESS;
}


DtlsTransportStatus_t DTLS_ExecuteHandshake(DtlsNetworkContext_t *pNetworkContext) {
    DtlsTransportParams_t *params;
    int rc;

    if (NULL == pNetworkContext || NULL == pNetworkContext->pParams) {
        return DTLS_INVALID_PARAMETER;
    }
    if (DTLS_STATE_READY == pNetworkContext->state) {
        return DTLS_HANDSHAKE_ALREADY_COMPLETE;
    }

    params = pNetworkContext->pParams;

    /* Pump handshake until mbedTLS either finishes, asks for more input, or
     * fails. WANT_WRITE means BIO send couldn't push everything — loop and
     * try again. */
    do {
        rc = mbedtls_ssl_handshake(&params->dtlsSslContext.context);
    } while (MBEDTLS_ERR_SSL_WANT_WRITE == rc);

    if (MBEDTLS_ERR_SSL_WANT_READ == rc) {
        return DTLS_SUCCESS;
    }
    if (rc < 0) {
        LogError(("mbedtls_ssl_handshake failed: -0x%04x", (unsigned int) -rc));
        return DTLS_TRANSPORT_HANDSHAKE_FAILED;
    }

    pNetworkContext->state = DTLS_STATE_READY;
    return DTLS_HANDSHAKE_COMPLETE;
}


int32_t DTLS_CreateCertificateAndKey(
    int32_t certificateBits,
    BaseType_t generateRSACertificate,
    mbedtls_x509_crt *pCert,
    mbedtls_pk_context *pKey
) {
    (void) generateRSACertificate;

    DtlsCompatWriteContext_t write_ctx;
    uint8_t serial_bytes[DTLS_CERT_SERIAL_LEN];
    unsigned char der[GENERATED_CERTIFICATE_MAX_SIZE];
    char not_before[16];
    char not_after[16];
    time_t now = time(NULL);
    int rc;

    if (NULL == pCert || NULL == pKey) {
        return DTLS_INVALID_PARAMETER;
    }
    if (0 != webrtc_csprng_init()) {
        return DTLS_GENERATE_RANDOM_BITS_FAILURE;
    }

    memset(&write_ctx, 0, sizeof(write_ctx));
    mbedtls_x509_crt_init(pCert);
    mbedtls_pk_init(pKey);
    mbedtls_x509write_crt_init(&write_ctx.cert);

    if (pdFALSE == generateRSACertificate) {
        rc = mbedtls_pk_setup(pKey, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
        if (0 != rc) {
            cleanup_write_context(&write_ctx);
            mbedtls_pk_free(pKey);
            mbedtls_x509_crt_free(pCert);
            return DTLS_INITIALIZE_PK_FAILURE;
        }

        rc = mbedtls_ecp_gen_key(
            MBEDTLS_ECP_DP_SECP256R1,
            mbedtls_pk_ec(*pKey),
            webrtc_csprng_f_rng,
            webrtc_csprng_p_rng()
        );
    } else {
        rc = mbedtls_pk_setup(pKey, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
        if (0 != rc) {
            cleanup_write_context(&write_ctx);
            mbedtls_pk_free(pKey);
            mbedtls_x509_crt_free(pCert);
            return DTLS_INITIALIZE_PK_FAILURE;
        }

        rc = mbedtls_rsa_gen_key(
            mbedtls_pk_rsa(*pKey),
            webrtc_csprng_f_rng,
            webrtc_csprng_p_rng(),
            (uint32_t) certificateBits,
            DTLS_RSA_F4
        );
    }
    if (0 != rc) {
        cleanup_write_context(&write_ctx);
        mbedtls_pk_free(pKey);
        mbedtls_x509_crt_free(pCert);
        return DTLS_GENERATE_KEY_FAILURE;
    }

    if (0 != webrtc_csprng_bytes(serial_bytes, sizeof(serial_bytes))) {
        cleanup_write_context(&write_ctx);
        mbedtls_pk_free(pKey);
        mbedtls_x509_crt_free(pCert);
        return DTLS_GENERATE_RANDOM_BITS_FAILURE;
    }
    serial_bytes[0] |= 0x01U;

    if (0 != format_validity_time(now, not_before) || 0 != format_validity_time(now + (GENERATED_CERTIFICATE_DAYS * DTLS_SECONDS_IN_A_DAY), not_after)) {
        cleanup_write_context(&write_ctx);
        mbedtls_pk_free(pKey);
        mbedtls_x509_crt_free(pCert);
        return DTLS_GENERATE_TIMESTAMP_STRING_FAILURE;
    }

    mbedtls_x509write_crt_set_md_alg(&write_ctx.cert, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&write_ctx.cert, pKey);
    mbedtls_x509write_crt_set_issuer_key(&write_ctx.cert, pKey);
    rc = mbedtls_x509write_crt_set_subject_name(&write_ctx.cert, "CN=" GENERATED_CERTIFICATE_NAME);
    if (0 != rc) {
        cleanup_write_context(&write_ctx);
        mbedtls_pk_free(pKey);
        mbedtls_x509_crt_free(pCert);
        return DTLS_SET_CERT_ISSUER_NAME_FAILURE;
    }
    rc = mbedtls_x509write_crt_set_issuer_name(&write_ctx.cert, "CN=" GENERATED_CERTIFICATE_NAME);
    if (0 != rc) {
        cleanup_write_context(&write_ctx);
        mbedtls_pk_free(pKey);
        mbedtls_x509_crt_free(pCert);
        return DTLS_SET_CERT_ISSUER_NAME_FAILURE;
    }
    rc = mbedtls_x509write_crt_set_serial_raw(&write_ctx.cert, serial_bytes, sizeof(serial_bytes));
    if (0 != rc) {
        cleanup_write_context(&write_ctx);
        mbedtls_pk_free(pKey);
        mbedtls_x509_crt_free(pCert);
        return DTLS_SET_CERT_SERIAL_FAILURE;
    }
    rc = mbedtls_x509write_crt_set_validity(&write_ctx.cert, not_before, not_after);
    if (0 != rc) {
        cleanup_write_context(&write_ctx);
        mbedtls_pk_free(pKey);
        mbedtls_x509_crt_free(pCert);
        return DTLS_SET_CERT_VALIDITY_FAILURE;
    }

    (void) mbedtls_x509write_crt_set_basic_constraints(&write_ctx.cert, 0, -1);
    (void) mbedtls_x509write_crt_set_subject_key_identifier(&write_ctx.cert);
    (void) mbedtls_x509write_crt_set_authority_key_identifier(&write_ctx.cert);
    (void) mbedtls_x509write_crt_set_key_usage(
        &write_ctx.cert,
        MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT
    );

    rc = mbedtls_x509write_crt_der(
        &write_ctx.cert,
        der,
        sizeof(der),
        webrtc_csprng_f_rng,
        webrtc_csprng_p_rng()
    );
    if (0 >= rc) {
        cleanup_write_context(&write_ctx);
        mbedtls_pk_free(pKey);
        mbedtls_x509_crt_free(pCert);
        return DTLS_WRITE_CERT_CRT_DER_FAILURE;
    }

    rc = mbedtls_x509_crt_parse_der(pCert, der + sizeof(der) - (size_t) rc, (size_t) rc);
    if (0 != rc) {
        cleanup_write_context(&write_ctx);
        mbedtls_pk_free(pKey);
        mbedtls_x509_crt_free(pCert);
        return DTLS_PARSE_CERT_DER_FAILURE;
    }

    cleanup_write_context(&write_ctx);
    return DTLS_SUCCESS;
}


int32_t DTLS_FreeCertificateAndKey(mbedtls_x509_crt *pCert, mbedtls_pk_context *pKey) {
    if (NULL == pCert || NULL == pKey) {
        return DTLS_INVALID_PARAMETER;
    }

    mbedtls_x509_crt_free(pCert);
    mbedtls_pk_free(pKey);
    return DTLS_SUCCESS;
}


int32_t DTLS_CreateCertificateFingerprint(const mbedtls_x509_crt *pCert, char *pBuff, const size_t bufLen) {
    mbedtls_sha256_context sha_ctx;
    uint8_t digest[DTLS_FINGERPRINT_SHA256_BYTES];
    int rc;

    if (NULL == pCert || NULL == pBuff || NULL == pCert->raw.p || 0U == pCert->raw.len) {
        return DTLS_INVALID_PARAMETER;
    }

    mbedtls_sha256_init(&sha_ctx);
    rc = mbedtls_sha256_starts(&sha_ctx, 0);
    if (0 == rc) {
        rc = mbedtls_sha256_update(&sha_ctx, pCert->raw.p, pCert->raw.len);
    }
    if (0 == rc) {
        rc = mbedtls_sha256_finish(&sha_ctx, digest);
    }
    mbedtls_sha256_free(&sha_ctx);
    if (0 != rc) {
        return DTLS_TRANSPORT_INTERNAL_ERROR;
    }
    if (0 != bytes_to_fingerprint_string(digest, sizeof(digest), pBuff, bufLen)) {
        return DTLS_OUT_OF_MEMORY;
    }

    return DTLS_SUCCESS;
}


int32_t DTLS_VerifyRemoteCertificateFingerprint(
    DtlsSSLContext_t *pSslContext,
    char *pExpectedFingerprint,
    const size_t fingerprintMaxLen
) {
    const mbedtls_x509_crt *peer_cert;
    char actual[DTLS_FINGERPRINT_STRING_LEN + 1U];
    size_t expected_len;

    if (NULL == pSslContext || NULL == pExpectedFingerprint) {
        return DTLS_INVALID_PARAMETER;
    }

    peer_cert = mbedtls_ssl_get_peer_cert(&pSslContext->context);
    if (NULL == peer_cert) {
        return DTLS_SSL_REMOTE_CERTIFICATE_VERIFICATION_FAILED;
    }
    if (DTLS_SUCCESS != DTLS_CreateCertificateFingerprint(peer_cert, actual, sizeof(actual))) {
        return DTLS_TRANSPORT_INTERNAL_ERROR;
    }

    expected_len = strnlen(pExpectedFingerprint, fingerprintMaxLen);
    if (expected_len != strlen(actual)) {
        return DTLS_SSL_REMOTE_CERTIFICATE_VERIFICATION_FAILED;
    }
    for (size_t i = 0; i < expected_len; i++) {
        if (toupper((unsigned char) pExpectedFingerprint[i]) != actual[i]) {
            return DTLS_SSL_REMOTE_CERTIFICATE_VERIFICATION_FAILED;
        }
    }

    return DTLS_SUCCESS;
}


int32_t DTLS_PopulateKeyingMaterial(DtlsSSLContext_t *pSslContext, pDtlsKeyingMaterial_t pDtlsKeyingMaterial) {
    TlsKeys *keys;
    mbedtls_dtls_srtp_info srtp_info;
    uint8_t keying_material[(MAX_SRTP_MASTER_KEY_LEN + MAX_SRTP_SALT_KEY_LEN) * 2U];
    size_t offset = 0U;
    int rc;

    if (NULL == pSslContext || NULL == pDtlsKeyingMaterial) {
        return DTLS_INVALID_PARAMETER;
    }

    keys = &pSslContext->tlsKeys;

    /* Derive SRTP keying material per RFC 5764 §4.2 using the
     * "EXTRACTOR-dtls_srtp" PRF label. mbedtls_ssl_tls_prf runs the same PRF
     * mbedTLS used during the handshake. */
    rc = mbedtls_ssl_tls_prf(keys->tlsProfile,
                             keys->masterSecret,
                             sizeof(keys->masterSecret),
                             KEYING_EXTRACTOR_LABEL,
                             keys->randBytes,
                             sizeof(keys->randBytes),
                             keying_material,
                             sizeof(keying_material));
    if (0 != rc) {
        LogError(("mbedtls_ssl_tls_prf failed: -0x%04x prf=%d", (unsigned int) -rc, (int) keys->tlsProfile));
        return DTLS_TRANSPORT_INTERNAL_ERROR;
    }

    /* Layout per RFC 5764: client_write_MK | server_write_MK | client_write_salt | server_write_salt. */
    pDtlsKeyingMaterial->key_length = MAX_SRTP_MASTER_KEY_LEN + MAX_SRTP_SALT_KEY_LEN;

    memcpy(pDtlsKeyingMaterial->clientWriteKey, &keying_material[offset], MAX_SRTP_MASTER_KEY_LEN);
    offset += MAX_SRTP_MASTER_KEY_LEN;
    memcpy(pDtlsKeyingMaterial->serverWriteKey, &keying_material[offset], MAX_SRTP_MASTER_KEY_LEN);
    offset += MAX_SRTP_MASTER_KEY_LEN;
    memcpy(pDtlsKeyingMaterial->clientWriteKey + MAX_SRTP_MASTER_KEY_LEN, &keying_material[offset], MAX_SRTP_SALT_KEY_LEN);
    offset += MAX_SRTP_SALT_KEY_LEN;
    memcpy(pDtlsKeyingMaterial->serverWriteKey + MAX_SRTP_MASTER_KEY_LEN, &keying_material[offset], MAX_SRTP_SALT_KEY_LEN);

    mbedtls_ssl_get_dtls_srtp_negotiation_result(&pSslContext->context, &srtp_info);

    switch (srtp_info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile)) {
        case MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80:
            pDtlsKeyingMaterial->srtpProfile = KVS_SRTP_PROFILE_AES128_CM_HMAC_SHA1_80;
            break;
        case MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_32:
            pDtlsKeyingMaterial->srtpProfile = KVS_SRTP_PROFILE_AES128_CM_HMAC_SHA1_32;
            break;
        default:
            LogError(("Unknown negotiated SRTP profile: %u", (unsigned int) srtp_info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile)));
            return DTLS_SSL_UNKNOWN_SRTP_PROFILE;
    }

    return DTLS_SUCCESS;
}
