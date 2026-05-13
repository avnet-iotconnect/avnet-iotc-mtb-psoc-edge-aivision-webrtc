/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"

#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

#include "csprng.h"
#include "transport_dtls_mbedtls.h"

#define DTLS_FINGERPRINT_SHA256_BYTES 32U
#define DTLS_FINGERPRINT_STRING_LEN ((DTLS_FINGERPRINT_SHA256_BYTES * 3U) - 1U)
#define DTLS_CERT_SERIAL_LEN 16U

typedef struct DtlsCompatWriteContext {
    mbedtls_x509write_cert cert;
} DtlsCompatWriteContext_t;


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


DtlsTransportStatus_t DTLS_Init(
    DtlsNetworkContext_t *pNetworkContext,
    DtlsNetworkCredentials_t *pNetworkCredentials,
    uint8_t isServer
) {
    (void) pNetworkCredentials;
    (void) isServer;

    if (NULL == pNetworkContext || NULL == pNetworkContext->pParams) {
        return DTLS_INVALID_PARAMETER;
    }
    if (0 != webrtc_csprng_init()) {
        return DTLS_GENERATE_RANDOM_BITS_FAILURE;
    }

    mbedtls_ssl_init(&pNetworkContext->pParams->dtlsSslContext.context);
    mbedtls_ssl_config_init(&pNetworkContext->pParams->dtlsSslContext.config);
    mbedtls_x509_crt_init(&pNetworkContext->pParams->dtlsSslContext.rootCa);
    mbedtls_x509_crt_init(&pNetworkContext->pParams->dtlsSslContext.clientCert);
    mbedtls_pk_init(&pNetworkContext->pParams->dtlsSslContext.privKey);
    mbedtls_entropy_init(&pNetworkContext->pParams->dtlsSslContext.entropyContext);
    mbedtls_ctr_drbg_init(&pNetworkContext->pParams->dtlsSslContext.ctrDrbgContext);
    pNetworkContext->state = DTLS_STATE_NEW;

    return DTLS_SUCCESS;
}


void DTLS_Disconnect(DtlsNetworkContext_t *pNetworkContext) {
    if (NULL == pNetworkContext || NULL == pNetworkContext->pParams) {
        return;
    }

    mbedtls_ssl_free(&pNetworkContext->pParams->dtlsSslContext.context);
    mbedtls_ssl_config_free(&pNetworkContext->pParams->dtlsSslContext.config);
    mbedtls_x509_crt_free(&pNetworkContext->pParams->dtlsSslContext.rootCa);
    mbedtls_x509_crt_free(&pNetworkContext->pParams->dtlsSslContext.clientCert);
    mbedtls_pk_free(&pNetworkContext->pParams->dtlsSslContext.privKey);
    mbedtls_entropy_free(&pNetworkContext->pParams->dtlsSslContext.entropyContext);
    mbedtls_ctr_drbg_free(&pNetworkContext->pParams->dtlsSslContext.ctrDrbgContext);
    pNetworkContext->state = DTLS_STATE_NONE;
}


int32_t DTLS_Send(DtlsNetworkContext_t *pNetworkContext, const void *pBuffer, size_t bytesToSend) {
    if (
        NULL == pNetworkContext ||
        NULL == pNetworkContext->pParams ||
        NULL == pNetworkContext->pParams->onDtlsSendHook ||
        NULL == pBuffer
    ) {
        return -1;
    }

    return pNetworkContext->pParams->onDtlsSendHook(
        pNetworkContext->pParams->pOnDtlsSendCustomContext,
        pBuffer,
        bytesToSend
    );
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
    (void) pNetworkContext;
    (void) pDtlsPacket;
    (void) dtlsPacketLength;
    (void) readBuffer;

    if (NULL == pReadBufferSize) {
        return DTLS_INVALID_PARAMETER;
    }
    *pReadBufferSize = 0U;
    return DTLS_TRANSPORT_PROCESS_FAILURE;
}


DtlsTransportStatus_t DTLS_ExecuteHandshake(DtlsNetworkContext_t *pNetworkContext) {
    (void) pNetworkContext;
    return DTLS_TRANSPORT_HANDSHAKE_FAILED;
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
    (void) pSslContext;
    (void) pDtlsKeyingMaterial;
    return DTLS_TRANSPORT_INTERNAL_ERROR;
}


