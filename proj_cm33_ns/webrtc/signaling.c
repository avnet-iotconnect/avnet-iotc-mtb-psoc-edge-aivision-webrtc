/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

/*
 * Signaling layer — Step 5a (GetSignalingChannelEndpoint) implemented;
 * Step 5b (ConnectAsViewer WSS) and message exchange remain stubs.
 *
 * GetSignalingChannelEndpoint flow:
 *   1. Signaling_ConstructGetSignalingChannelEndpointRequest  — builds URL + body
 *   2. SigV4_GenerateHTTPAuthorization                       — builds Authorization header
 *   3. iotconnect_https_request_with_opts                    — POST, returns JSON
 *   4. Signaling_ParseGetSignalingChannelEndpointResponse    — extracts WSS endpoint
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"

#include "mbedtls/sha256.h"

#include "sigv4.h"
#include "signaling_api.h"

#include "iotcl_certs.h"
#include "iotcl_dra_url.h"
#include "iotc_http_client.h"

#include "webrtc/signaling.h"

// -------------------------------------------------------------------------
// Buffer sizes
// -------------------------------------------------------------------------

// Full URL from the Signaling lib: "https://kinesisvideo.<region>.amazonaws.com/..."
// Longest practical: ~80 chars; 192 gives comfortable room.
#define SIG_URL_BUF         192

// JSON body for GetSignalingChannelEndpoint — ARN + protocol/role fields.
// ARN is ~80 chars; total body is well under 512.
#define SIG_BODY_BUF        512

// ISO 8601 date: "YYYYMMDDTHHMMSSz" — exactly 16 chars + null.
#define SIG_DATE_ISO_LEN    17

// Authorization header value output from SigV4_GenerateHTTPAuthorization.
// "AWS4-HMAC-SHA256 Credential=.../kinesisvideo/aws4_request, SignedHeaders=..., Signature=<64>"
// ~350 chars; 512 is safe.
#define SIG_AUTH_BUF        512

// KVS service name for SigV4.
#define KVS_SERVICE         "kinesisvideo"
#define KVS_SERVICE_LEN     (sizeof(KVS_SERVICE) - 1)

// Overhead bytes in the canonical headers string beyond the three variable
// fields (host, date, token): field names + colons + newlines + null.
// "host:\nx-amz-date:\nx-amz-security-token:\n\0" = ~57 chars; 64 is safe.
#define SIG_CANON_HDR_OVERHEAD 64

// File-scope statics for predictable-size scratch used inside
// signaling_resolve_endpoint. One call at a time (single WebRTC task).
static char s_url_buf[SIG_URL_BUF];
static char s_body_buf[SIG_BODY_BUF];
static char s_auth_buf[SIG_AUTH_BUF + 1]; // +1: null-terminate after SigV4 fills SIG_AUTH_BUF bytes
static mbedtls_sha256_context s_sha_ctx;

// -------------------------------------------------------------------------
// mbedTLS SHA-256 callbacks (same pattern as webrtc_smoke_test.c)
// -------------------------------------------------------------------------

static int32_t sha256_init_cb(void *ctx) {
    mbedtls_sha256_init((mbedtls_sha256_context *) ctx);
    return mbedtls_sha256_starts((mbedtls_sha256_context *) ctx, 0);
}

static int32_t sha256_update_cb(void *ctx, const uint8_t *in, size_t len) {
    return mbedtls_sha256_update((mbedtls_sha256_context *) ctx, in, len);
}

static int32_t sha256_final_cb(void *ctx, uint8_t *out, size_t outlen) {
    (void) outlen;
    return mbedtls_sha256_finish((mbedtls_sha256_context *) ctx, out);
}


// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

// Format the current UTC time as ISO 8601 for SigV4 ("YYYYMMDDTHHMMSSz").
// Writes exactly 16 chars + null terminator into buf[SIG_DATE_ISO_LEN].
static void format_iso8601_now(char buf[SIG_DATE_ISO_LEN]) {
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    strftime(buf, SIG_DATE_ISO_LEN, "%Y%m%dT%H%M%SZ", &t);
}

int signaling_resolve_endpoint(const AwsCreds *creds, char *out_endpoint, size_t cap) {
    if (NULL == creds || NULL == out_endpoint || 0 == cap) {
        return -1;
    }

    // 1. Build URL + body via the Signaling library.
    SignalingAwsRegion_t region = {
        .pAwsRegion      = creds->region,
        .awsRegionLength = strlen(creds->region),
    };
    GetSignalingChannelEndpointRequestInfo_t req_info = {
        .channelArn = {
            .pChannelArn      = creds->channel_arn,
            .channelArnLength = strlen(creds->channel_arn),
        },
        .protocols = SIGNALING_PROTOCOL_WEBSOCKET_SECURE,
        .role      = SIGNALING_ROLE_VIEWER,
    };
    SignalingRequest_t sig_req = {
        .pUrl       = s_url_buf,
        .urlLength  = sizeof(s_url_buf),
        .pBody      = s_body_buf,
        .bodyLength = sizeof(s_body_buf),
    };

    SignalingResult_t sig_rc = Signaling_ConstructGetSignalingChannelEndpointRequest(
        &region, &req_info, &sig_req
    );
    if (SIGNALING_RESULT_OK != sig_rc) {
        printf("[signaling] ConstructGetSignalingChannelEndpointRequest failed: %d\n", (int) sig_rc);
        return -1;
    }
    // urlLength and bodyLength are now the actual written lengths (no null).
    // s_url_buf and s_body_buf are null-terminated by snprintf inside the lib.

    // 2. Parse host and resource path from the URL via the DRA URL module.
    //    IotclDraUrlContext malloc's the hostname and a copy of the URL.
    //    Zero-initialise so deinit is safe even if init fails.
    IotclDraUrlContext url_ctx = {0};
    if (0 != iotcl_dra_url_init(&url_ctx, s_url_buf)) {
        printf("[signaling] failed to parse URL from signaling lib: %s\n", s_url_buf);
        return -1;
    }
    const char *host = iotcl_dra_url_get_hostname(&url_ctx);
    const char *path = iotcl_dra_url_get_resource(&url_ctx);
    if (NULL == host || NULL == path) {
        printf("[signaling] URL missing host or path: %s\n", s_url_buf);
        iotcl_dra_url_deinit(&url_ctx);
        return -1;
    }

    int rc = 0;
    char *canon_hdr_buf = NULL;
    IotConnectHttpResponse response = { .data = NULL };

    // 3. Format current UTC time as ISO 8601.
    char date_iso[SIG_DATE_ISO_LEN];
    format_iso8601_now(date_iso);

    // 4. Build canonical headers string for SigV4 signing.
    //    Canonical form: "<name>:<value>\n" sorted lexicographically.
    //    We sign: host, x-amz-date, x-amz-security-token (sorted order).
    //    session_token varies (~1200 chars for STS tokens) — malloc to exact fit.
    size_t token_len = creds->session_token ? strlen(creds->session_token) : 0;
    size_t canon_hdr_size = strlen(host) + sizeof(date_iso) + token_len + SIG_CANON_HDR_OVERHEAD;
    canon_hdr_buf = malloc(canon_hdr_size);
    if (NULL == canon_hdr_buf) {
        printf("[signaling] OOM for canon_hdr_buf (%d bytes)\n", (int) canon_hdr_size);
        rc = -1;
        goto cleanup;
    }
    int hdr_len = snprintf(
        canon_hdr_buf, canon_hdr_size,
        "host:%s\n"
        "x-amz-date:%s\n"
        "x-amz-security-token:%s\n",
        host, date_iso,
        creds->session_token ? creds->session_token : ""
    );
    if (hdr_len <= 0 || (size_t) hdr_len >= canon_hdr_size) {
        printf("[signaling] canon_hdr_buf overflow — SIG_CANON_HDR_OVERHEAD too small\n");
        rc = -1;
        goto cleanup;
    }

    // 5. Sign with SigV4.
    SigV4CryptoInterface_t crypto = {
        .hashInit      = sha256_init_cb,
        .hashUpdate    = sha256_update_cb,
        .hashFinal     = sha256_final_cb,
        .pHashContext  = &s_sha_ctx,
        .hashBlockLen  = 64,
        .hashDigestLen = 32,
    };
    SigV4Credentials_t sigv4_creds = {
        .pAccessKeyId       = creds->access_key_id,
        .accessKeyIdLen     = strlen(creds->access_key_id),
        .pSecretAccessKey   = creds->secret_access_key,
        .secretAccessKeyLen = strlen(creds->secret_access_key),
    };
    SigV4HttpParameters_t http_params = {
        .pHttpMethod   = "POST",
        .httpMethodLen = 4,
        .flags         = SIGV4_HTTP_PATH_IS_CANONICAL_FLAG
                       | SIGV4_HTTP_HEADERS_ARE_CANONICAL_FLAG,
        .pPath    = path,
        .pathLen  = strlen(path),
        .pQuery   = NULL,
        .queryLen = 0,
        .pHeaders   = canon_hdr_buf,
        .headersLen = (size_t) hdr_len,
        .pPayload   = s_body_buf,
        .payloadLen = sig_req.bodyLength,
    };
    SigV4Parameters_t sigv4_params = {
        .pCredentials     = &sigv4_creds,
        .pDateIso8601     = date_iso,
        .pAlgorithm       = NULL, // defaults to AWS4-HMAC-SHA256
        .algorithmLen     = 0,
        .pRegion          = creds->region,
        .regionLen        = strlen(creds->region),
        .pService         = KVS_SERVICE,
        .serviceLen       = KVS_SERVICE_LEN,
        .pCryptoInterface = &crypto,
        .pHttpParameters  = &http_params,
    };

    size_t auth_buf_len = SIG_AUTH_BUF; // reserve the +1 byte for null terminator
    char *sig_ptr = NULL;
    size_t sig_len = 0;
    SigV4Status_t sv4_rc = SigV4_GenerateHTTPAuthorization(
        &sigv4_params, s_auth_buf, &auth_buf_len, &sig_ptr, &sig_len
    );
    if (SigV4Success != sv4_rc) {
        printf("[signaling] SigV4_GenerateHTTPAuthorization failed: %d\n", (int) sv4_rc);
        rc = -1;
        goto cleanup;
    }
    s_auth_buf[auth_buf_len] = '\0';

    // 6. POST via iotc_http_client with Authorization + date + security-token headers.
    IotConnectHttpHeader extra_headers[3] = {
        { .name = "Authorization",        .value = s_auth_buf },
        { .name = "x-amz-date",           .value = date_iso   },
        { .name = "x-amz-security-token", .value = (char *) (creds->session_token ? creds->session_token : "") },
    };
    IotConnectHttpOpts opts = {
        .ca_cert     = (char *) IOTCL_AMAZON_ROOT_CA1,
        .cert        = NULL,
        .key         = NULL,
        .headers     = extra_headers,
        .headers_len = 3,
    };

    unsigned int http_rc = iotconnect_https_request_with_opts(
        &response, host, path, s_body_buf, &opts
    );
    if (0 != http_rc || NULL == response.data) {
        printf("[signaling] GetSignalingChannelEndpoint HTTP failed: 0x%08x\n", http_rc);
        rc = -1;
        goto cleanup;
    }

    // 7. Parse the response to extract the WSS endpoint.
    SignalingChannelEndpoints_t endpoints = { 0 };
    sig_rc = Signaling_ParseGetSignalingChannelEndpointResponse(
        response.data, strlen(response.data), &endpoints
    );
    if (SIGNALING_RESULT_OK != sig_rc) {
        printf("[signaling] ParseGetSignalingChannelEndpointResponse failed: %d\n", (int) sig_rc);
        rc = -1;
        goto cleanup;
    }
    if (NULL == endpoints.wssEndpoint.pEndpoint || 0 == endpoints.wssEndpoint.endpointLength) {
        printf("[signaling] no WSS endpoint in response\n");
        rc = -1;
        goto cleanup;
    }
    // The endpoint pointer is into response.data — copy before freeing.
    if (endpoints.wssEndpoint.endpointLength >= cap) {
        printf("[signaling] WSS endpoint too long for caller buffer (%d >= %d)\n",
            (int) endpoints.wssEndpoint.endpointLength, (int) cap);
        rc = -1;
        goto cleanup;
    }
    memcpy(out_endpoint, endpoints.wssEndpoint.pEndpoint, endpoints.wssEndpoint.endpointLength);
    out_endpoint[endpoints.wssEndpoint.endpointLength] = '\0';
    printf("[signaling] WSS endpoint: %s\n", out_endpoint);

cleanup:
    iotcl_dra_url_deinit(&url_ctx);
    free(canon_hdr_buf);
    iotconnect_free_https_response(&response);
    return rc;
}

// -------------------------------------------------------------------------
// Step 5b stubs — ConnectAsViewer, message exchange
// -------------------------------------------------------------------------

struct SignalingCtx {
    int dummy;
};

static struct SignalingCtx g_sig;

int signaling_build_signed_viewer_url(
    const AwsCreds *creds,
    const char *wss_endpoint,
    char *out_url,
    size_t cap
) {
    (void) creds;
    printf("[signaling] signaling_build_signed_viewer_url: STUB (endpoint=%s)\n",
        wss_endpoint ? wss_endpoint : "<null>");
    if (cap > 0) {
        snprintf(out_url, cap, "wss://%s/?role=VIEWER&...sigv4...",
            wss_endpoint ? wss_endpoint : "?");
    }
    return -1; // stub — returns error so caller backs off
}

SignalingHandle signaling_connect(const char *signed_url) {
    (void) signed_url;
    printf("[signaling] signaling_connect: STUB\n");
    return &g_sig;
}

void signaling_disconnect(SignalingHandle sig) {
    (void) sig;
    printf("[signaling] signaling_disconnect: STUB\n");
}

int signaling_wait_for_offer(SignalingHandle sig, const char **out_offer) {
    (void) sig;
    printf("[signaling] signaling_wait_for_offer: STUB\n");
    if (out_offer) {
        *out_offer = NULL;
    }
    return -1;
}

int signaling_send_answer(SignalingHandle sig, const char *sdp_answer) {
    (void) sig;
    (void) sdp_answer;
    printf("[signaling] signaling_send_answer: STUB\n");
    return -1;
}
