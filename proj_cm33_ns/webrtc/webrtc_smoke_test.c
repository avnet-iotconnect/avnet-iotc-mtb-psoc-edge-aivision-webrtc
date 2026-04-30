/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

/* Smoke tests for WebRTC third-party libs.  Pure computation; no network,
 * no RTOS primitives.  Each test prints PASS/FAIL and returns the fail count.
 * Purpose: verify that all libs link correctly and that the most basic API
 * paths work before any real integration code is written.
 *
 * Tests:
 *  1. SigV4   -- sign a presigned KVS ConnectAsViewer URL with fake 1-hour creds
 *  2. STUN    -- serialize a Binding Request, deserialize it back
 *  3. H264    -- packetize a single synthetic NAL unit, get one RTP packet
 *  4. SDP     -- build a minimal SDP session description
 *  5. libsrtp -- srtp_init() (one-time library init)
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

/* SigV4 */
#include "sigv4.h"
#include "mbedtls/sha256.h"

/* STUN */
#include "stun_serializer.h"
#include "stun_deserializer.h"

/* H.264 / RTP */
#include "h264_packetizer.h"

/* SDP */
#include "sdp_serializer.h"

/* libsrtp */
#include "srtp.h"

#include "webrtc_smoke_test.h"

// for vTaskDelay
#include "FreeRTOS.h"
#include "task.h"
/* -------------------------------------------------------------------------- */

#define TEST_PASS(name, ...)  printf("[SMOKE] PASS  " name "\n", ##__VA_ARGS__)
#define TEST_FAIL(name, fmt, ...) printf("[SMOKE] FAIL  " name " -- " fmt "\n", ##__VA_ARGS__)

/* -------------------------------------------------------------------------- */
/* 1. SigV4 presigned-URL signing                                             */
/* -------------------------------------------------------------------------- */

/* mbedTLS SHA-256 callbacks for the SigV4 crypto interface. */
static int32_t sha256_init_cb(void *ctx) {
    mbedtls_sha256_init((mbedtls_sha256_context *)ctx);
    return mbedtls_sha256_starts((mbedtls_sha256_context *)ctx, 0 /* sha256, not sha224 */);
}

static int32_t sha256_update_cb(void *ctx, const uint8_t *in, size_t len) {
    return mbedtls_sha256_update((mbedtls_sha256_context *)ctx, in, len);
}

static int32_t sha256_final_cb(void *ctx, uint8_t *out, size_t outlen) {
    (void)outlen; /* caller guarantees >= 32 */
    return mbedtls_sha256_finish((mbedtls_sha256_context *)ctx, out);
}

static int test_sigv4(void) {
    /* Fake 1-hour creds.  AKID must be 16-128 chars; secret >= 40 chars. */
    static const char AKID[]   = "TESTAKIDTESTAKID1234";   /* 20 chars  */
    static const char SECRET[] = "testsecrettestsecrettestsecrettestsecret"; /* 40 chars */

    /* ISO 8601 date: exactly 16 chars, no null terminator used by API. */
    static const char DATE_ISO[] = "20260430T120000Z";

    /* Pre-canonical (already sorted, URI-encoded) query parameters.
     * Alphabetical order is required; we skip security token for simplicity
     * since we're testing the signing mechanic, not the full KVS query set. */
    static const char CANON_QUERY[] =
        "X-Amz-Algorithm=AWS4-HMAC-SHA256"
        "&X-Amz-Credential=TESTAKIDTESTAKID1234%2F20260430%2Fus-east-1%2Fkinesisvideo%2Faws4_request"
        "&X-Amz-Date=20260430T120000Z"
        "&X-Amz-Expires=300"
        "&X-Amz-SignedHeaders=host";

    /* Pre-canonical headers: only "host" is signed (required for KVS). */
    static const char CANON_HEADERS[] =
        "host:test-endpoint.kinesisvideo.us-east-1.amazonaws.com\n";

    /* Output buffer: must hold Authorization prefix + 64-char hex signature.
     * Prefix = "AWS4-HMAC-SHA256 Credential=<AKID>/<scope>, SignedHeaders=host, Signature="
     * ≈ 180 bytes; 512 bytes is comfortable.
     * static: keeps it in .bss, not on the stack; SigV4 already consumes ~1 KB
     * of stack internally (CanonicalContext_t.pBufProcessing[1024]). */
    static char auth_buf[512];
    size_t auth_buf_len = sizeof(auth_buf);
    char *sig_ptr = NULL;
    size_t sig_len = 0;

    static mbedtls_sha256_context sha_ctx;
    SigV4CryptoInterface_t crypto = {
        .hashInit     = sha256_init_cb,
        .hashUpdate   = sha256_update_cb,
        .hashFinal    = sha256_final_cb,
        .pHashContext = &sha_ctx,
        .hashBlockLen  = 64,
        .hashDigestLen = 32,
    };

    SigV4Credentials_t creds = {
        .pAccessKeyId      = AKID,
        .accessKeyIdLen    = sizeof(AKID) - 1,
        .pSecretAccessKey  = SECRET,
        .secretAccessKeyLen = sizeof(SECRET) - 1,
    };

    SigV4HttpParameters_t http_params = {
        .pHttpMethod  = "GET",
        .httpMethodLen = 3,
        .flags         = SIGV4_HTTP_IS_PRESIGNED_URL
                       | SIGV4_HTTP_QUERY_IS_CANONICAL_FLAG
                       | SIGV4_HTTP_HEADERS_ARE_CANONICAL_FLAG
                       | SIGV4_HTTP_PATH_IS_CANONICAL_FLAG,
        .pPath    = "/connectAsViewer",
        .pathLen  = 16,
        .pQuery   = CANON_QUERY,
        .queryLen = sizeof(CANON_QUERY) - 1,
        .pHeaders    = CANON_HEADERS,
        .headersLen  = sizeof(CANON_HEADERS) - 1,
        .pPayload  = NULL,
        .payloadLen = 0,
    };

    SigV4Parameters_t params = {
        .pCredentials    = &creds,
        .pDateIso8601    = DATE_ISO,
        .pAlgorithm      = NULL, /* default: AWS4-HMAC-SHA256 */
        .algorithmLen    = 0,
        .pRegion         = "us-east-1",
        .regionLen       = 9,
        .pService        = "kinesisvideo",
        .serviceLen      = 12,
        .pCryptoInterface = &crypto,
        .pHttpParameters  = &http_params,
    };

    SigV4Status_t rc = SigV4_GenerateHTTPAuthorization(
        &params, auth_buf, &auth_buf_len, &sig_ptr, &sig_len
    );
    if (rc != SigV4Success) {
        TEST_FAIL("sigv4", "SigV4_GenerateHTTPAuthorization returned %d", (int)rc);
        return 1;
    }
    if (sig_len != 64) {
        TEST_FAIL("sigv4", "expected 64-char hex signature, got %d", (int)sig_len);
        return 1;
    }
    printf("[SMOKE] PASS  sigv4  X-Amz-Signature=%.*s\n", (int)sig_len, sig_ptr);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* 2. STUN serialize / deserialize round-trip                                 */
/* -------------------------------------------------------------------------- */

static int test_stun(void) {
    uint8_t buf[64];
    uint8_t txid[STUN_HEADER_TRANSACTION_ID_LENGTH] = {
        0x01,0x02,0x03,0x04, 0x05,0x06,0x07,0x08, 0x09,0x0A,0x0B,0x0C
    };

    StunHeader_t hdr = {
        .messageType    = STUN_MESSAGE_TYPE_BINDING_REQUEST,
        .pTransactionId = txid,
    };
    StunContext_t ctx;
    size_t msg_len = 0;

    StunResult_t rc = StunSerializer_Init(&ctx, buf, sizeof(buf), &hdr);
    if (rc != STUN_RESULT_OK) {
        TEST_FAIL("stun/serialize", "StunSerializer_Init returned %d", (int)rc);
        return 1;
    }
    rc = StunSerializer_Finalize(&ctx, &msg_len);
    if (rc != STUN_RESULT_OK) {
        TEST_FAIL("stun/serialize", "StunSerializer_Finalize returned %d", (int)rc);
        return 1;
    }
    if (msg_len < STUN_HEADER_LENGTH) {
        TEST_FAIL("stun/serialize", "message too short: %d", (int)msg_len);
        return 1;
    }

    /* Round-trip: deserialize the serialized buffer. */
    StunHeader_t parsed_hdr;
    StunContext_t dctx;
    rc = StunDeserializer_Init(&dctx, buf, msg_len, &parsed_hdr);
    if (rc != STUN_RESULT_OK) {
        TEST_FAIL("stun/deserialize", "StunDeserializer_Init returned %d", (int)rc);
        return 1;
    }
    if (parsed_hdr.messageType != STUN_MESSAGE_TYPE_BINDING_REQUEST) {
        TEST_FAIL("stun/deserialize", "messageType mismatch: got 0x%04x", (unsigned)parsed_hdr.messageType);
        return 1;
    }
    if (memcmp(parsed_hdr.pTransactionId, txid, STUN_HEADER_TRANSACTION_ID_LENGTH) != 0) {
        TEST_FAIL("stun/deserialize", "transaction ID mismatch");
        return 1;
    }
    TEST_PASS("stun  (serialize + round-trip, %d bytes)", (int)msg_len);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* 3. H.264 packetizer: add one NAL, get one packet                          */
/* -------------------------------------------------------------------------- */

static int test_h264_packetizer(void) {
    /* Minimal IDR NAL: header byte 0x65 (NRI=3, type=5) + 8 bytes payload. */
    static uint8_t nal_data[] = { 0x65, 0x88, 0x84, 0x00, 0x21, 0xFF, 0xAB, 0xCD, 0xEF };

    Nalu_t nalu_array[4];
    H264PacketizerContext_t pctx;
    H264Result_t rc = H264Packetizer_Init(&pctx, nalu_array, 4);
    if (rc != H264_RESULT_OK) {
        TEST_FAIL("h264/init", "H264Packetizer_Init returned %d", (int)rc);
        return 1;
    }

    Nalu_t nalu = { .pNaluData = nal_data, .naluDataLength = sizeof(nal_data) };
    rc = H264Packetizer_AddNalu(&pctx, &nalu);
    if (rc != H264_RESULT_OK) {
        TEST_FAIL("h264/add", "H264Packetizer_AddNalu returned %d", (int)rc);
        return 1;
    }

    /* GetPacket writes INTO pPacketData — caller must supply the output buffer. */
    static uint8_t pkt_buf[128];
    H264Packet_t pkt = { .pPacketData = pkt_buf, .packetDataLength = sizeof(pkt_buf) };
    rc = H264Packetizer_GetPacket(&pctx, &pkt);
    if (rc != H264_RESULT_OK) {
        TEST_FAIL("h264/get", "H264Packetizer_GetPacket returned %d", (int)rc);
        return 1;
    }
    if (pkt.packetDataLength == 0) {
        TEST_FAIL("h264/get", "empty packet");
        return 1;
    }
    TEST_PASS("h264  (single NAL → %d-byte RTP payload)", (int)pkt.packetDataLength);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* 4. SDP serializer: build a minimal SDP session description                */
/* -------------------------------------------------------------------------- */

static int test_sdp(void) {
    char sdp_buf[256];
    SdpSerializerContext_t sctx;

    SdpResult_t rc = SdpSerializer_Init(&sctx, sdp_buf, sizeof(sdp_buf));
    if (rc != SDP_RESULT_OK) {
        TEST_FAIL("sdp/init", "SdpSerializer_Init returned 0x%x", (unsigned)rc);
        return 1;
    }

    /* v=0 */
    rc = SdpSerializer_AddU32(&sctx, SDP_TYPE_VERSION, 0);
    if (rc != SDP_RESULT_OK) {
        TEST_FAIL("sdp/version", "AddU32 returned 0x%x", (unsigned)rc);
        return 1;
    }

    /* s=KVS-WebRTC-Smoke */
    static const char SNAME[] = "KVS-WebRTC-Smoke";
    rc = SdpSerializer_AddBuffer(&sctx, SDP_TYPE_SESSION_NAME, SNAME, sizeof(SNAME) - 1);
    if (rc != SDP_RESULT_OK) {
        TEST_FAIL("sdp/session-name", "AddBuffer returned 0x%x", (unsigned)rc);
        return 1;
    }

    const char *out = NULL;
    size_t out_len = 0;
    rc = SdpSerializer_Finalize(&sctx, &out, &out_len);
    if (rc != SDP_RESULT_OK) {
        TEST_FAIL("sdp/finalize", "SdpSerializer_Finalize returned 0x%x", (unsigned)rc);
        return 1;
    }
    if (out_len == 0 || out == NULL) {
        TEST_FAIL("sdp/finalize", "empty output");
        return 1;
    }
    TEST_PASS("sdp   (%d bytes)", (int)out_len);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* 5. libsrtp: library init                                                   */
/* -------------------------------------------------------------------------- */

static int test_srtp(void) {
    srtp_err_status_t rc = srtp_init();
    /* srtp_init() is idempotent if called multiple times; libraries that call
     * it earlier (e.g. from static constructors) can make this return a
     * "already initialized" code on some ports, but cisco/libsrtp just returns
     * srtp_err_status_ok on repeat calls. */
    if (rc != srtp_err_status_ok) {
        TEST_FAIL("srtp", "srtp_init() returned %d", (int)rc);
        return 1;
    }
    TEST_PASS("srtp  (srtp_init ok)");
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                 */
/* -------------------------------------------------------------------------- */

int webrtc_smoke_test_run(void) {
    int fails = 0;
    vTaskDelay(2000);
    printf("[SMOKE] ---- WebRTC library smoke tests ----\n");
    fails += test_sigv4();
    fails += test_stun();
    printf("[SMOKE] ---- Doing H.264 packetizer test ----\n");
    fails += test_h264_packetizer();
    printf("[SMOKE] ---- Doing SDP serializer test ----\n");
    fails += test_sdp();
    fails += test_srtp();
    if (fails == 0) {
        printf("[SMOKE] ---- ALL PASSED (%d/5) ----\n", 5);
    } else {
        printf("[SMOKE] ---- %d FAILED ----\n", fails);
    }
    return fails;
}
