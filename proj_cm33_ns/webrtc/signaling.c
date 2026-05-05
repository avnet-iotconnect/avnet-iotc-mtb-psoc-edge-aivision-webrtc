/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

/*
 * Signaling layer.
 *
 * Step 5a (GetSignalingChannelEndpoint, REST) and Step 5b URL signing
 * (signaling_build_signed_viewer_url) are implemented. The WSS handshake +
 * frame exchange (signaling_connect / wait_for_offer / send_answer) are stubs.
 *
 * GetSignalingChannelEndpoint flow:
 *   1. Signaling_ConstructGetSignalingChannelEndpointRequest  — builds URL + body
 *   2. SigV4_GenerateHTTPAuthorization                       — builds Authorization header
 *   3. iotconnect_https_request_with_opts                    — POST, returns JSON
 *   4. Signaling_ParseGetSignalingChannelEndpointResponse    — extracts WSS endpoint
 *
 * ConnectAsViewer URL-signing flow (signaling_build_signed_viewer_url):
 *   1. Signaling_ConstructConnectWssEndpointRequest  — builds the unsigned base URL
 *   2. Build canonical query string with X-Amz-* params (URI-encoded, lex-sorted)
 *   3. SigV4_GenerateHTTPAuthorization                — signs the canonical request
 *   4. Append &X-Amz-Signature=<sig> to the query
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

// Full URL from the Signaling lib.
// 5a (HTTPS): "https://kinesisvideo.<region>.amazonaws.com/..." — ~80 chars.
// 5b base URL (WSS): "wss://<host>?X-Amz-ChannelARN=<arn>&X-Amz-ClientId=<id>"
// Host ~50, ARN ~110, client_id up to ~64 → up to ~280. 384 leaves comfortable room.
#define SIG_URL_BUF         384

// JSON body for GetSignalingChannelEndpoint — ARN + protocol/role fields.
// ARN is ~80 chars; total body is well under 512.
#define SIG_BODY_BUF        512

// ISO 8601 date: "YYYYMMDDTHHMMSSz" — exactly 16 chars + null.
#define SIG_DATE_ISO_LEN    17

// Authorization header value output from SigV4_GenerateHTTPAuthorization.
// "AWS4-HMAC-SHA256 Credential=.../kinesisvideo/aws4_request, SignedHeaders=..., Signature=<64>"
// Real KVS values run ~380-450 chars; 1024 leaves comfortable headroom.
#define SIG_AUTH_BUF        1024

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
// SigV4_GenerateHTTPAuthorization treats authBufLen as in/out: input is capacity,
// output is bytes actually written (always <= capacity). We pass SIG_AUTH_BUF in,
// then null-terminate at the returned length — the +1 byte covers the case where
// SigV4 fills the buffer completely and we still need somewhere for the '\0'.
static char s_auth_buf[SIG_AUTH_BUF + 1];
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
    size_t canon_hdr_size = strlen(host) + sizeof(date_iso) + strlen(creds->session_token) + SIG_CANON_HDR_OVERHEAD;
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
        host, date_iso, creds->session_token
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
        { .name = "x-amz-security-token", .value = (char *) creds->session_token },
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
// Step 5b — build SigV4-presigned ConnectAsViewer URL
// -------------------------------------------------------------------------

// Append a literal string fragment to a write cursor, advancing it. Returns 0
// on success, -1 if the fragment doesn't fit (including no room for the '\0'
// we'll write later — caller controls termination).
static int append_literal(char **cur, size_t *remain, const char *s) {
    size_t n = strlen(s);
    if (n >= *remain) {
        return -1;
    }
    memcpy(*cur, s, n);
    *cur += n;
    *remain -= n;
    return 0;
}

// Append a URI-encoded value to a write cursor using SigV4_EncodeURI.
// encode_slash=true (RFC 3986 "encode all reserved chars in values").
static int append_uri_encoded(char **cur, size_t *remain, const char *value, size_t value_len) {
    size_t encoded_len = *remain;
    SigV4Status_t rc = SigV4_EncodeURI(value, value_len, *cur, &encoded_len, true, false);
    if (SigV4Success != rc) {
        return -1;
    }
    *cur += encoded_len;
    *remain -= encoded_len;
    return 0;
}

// Strip the "wss://" or "https://" scheme from an endpoint URL. Returns a
// pointer into the input string just past the scheme delimiter.
static const char *strip_scheme(const char *url) {
    const char *p = strstr(url, "://");
    return (NULL == p) ? url : (p + 3);
}

int signaling_build_signed_viewer_url(
    const AwsCreds *creds,
    const char *wss_endpoint,
    char *out_url,
    size_t cap
) {
    if (NULL == creds || NULL == wss_endpoint || NULL == out_url || 0 == cap) {
        return -1;
    }
    if (NULL == creds->client_id || '\0' == creds->client_id[0]) {
        printf("[signaling] missing client_id for X-Amz-ClientId\n");
        return -1;
    }
    if (NULL == creds->session_token) {
        // STS triplet from /IOTCONNECT always includes a session token. If
        // it's missing something has gone wrong upstream; fail loudly.
        printf("[signaling] missing session token\n");
        return -1;
    }

    // 1. Build the unsigned base URL via the Signaling lib. Same idiom as
    //    signaling_resolve_endpoint — keeps the URL skeleton lib-blessed.
    SignalingChannelEndpoint_t wss_ep = {
        .pEndpoint      = (char *) wss_endpoint,
        .endpointLength = strlen(wss_endpoint),
    };
    ConnectWssEndpointRequestInfo_t wss_info = {
        .channelArn = {
            .pChannelArn      = creds->channel_arn,
            .channelArnLength = strlen(creds->channel_arn),
        },
        .role           = SIGNALING_ROLE_VIEWER,
        .pClientId      = creds->client_id,
        .clientIdLength = strlen(creds->client_id),
    };
    SignalingRequest_t sig_req = {
        .pUrl       = s_url_buf,
        .urlLength  = sizeof(s_url_buf),
        .pBody      = NULL,
        .bodyLength = 0,
    };
    SignalingResult_t sig_rc = Signaling_ConstructConnectWssEndpointRequest(
        &wss_ep, &wss_info, &sig_req
    );
    if (SIGNALING_RESULT_OK != sig_rc) {
        printf("[signaling] ConstructConnectWssEndpointRequest failed: %d\n", (int) sig_rc);
        return -1;
    }

    // 2. Pull the bare host (no scheme) from the base URL. The lib output is
    //    "wss://<host>?X-Amz-ChannelARN=...&X-Amz-ClientId=..." — the host is
    //    everything between "://" and '?'. We need it for the SigV4 host header
    //    *and* for the final "wss://<host>/?<query>" composition.
    const char *host_start = strip_scheme(s_url_buf);
    const char *host_end   = strchr(host_start, '?');
    if (NULL == host_end) {
        printf("[signaling] base URL missing '?': %s\n", s_url_buf);
        return -1;
    }
    size_t host_len = (size_t)(host_end - host_start);
    if (0 == host_len) {
        printf("[signaling] empty host in base URL\n");
        return -1;
    }

    // 3. ISO 8601 date.
    char date_iso[SIG_DATE_ISO_LEN];
    format_iso8601_now(date_iso);

    // 4. Build the canonical query string directly into out_url. The query
    //    starts after "wss://<host>/?" — we'll prepend that prefix in step 8.
    //    For now write the query bytes starting at out_url + 0; we shift them
    //    later to make room for the prefix. Cleaner than a separate buffer.
    //    Order is lex-sorted (SigV4 requirement when QUERY_IS_CANONICAL is set):
    //      X-Amz-Algorithm, X-Amz-ChannelARN, X-Amz-ClientId, X-Amz-Credential,
    //      X-Amz-Date, X-Amz-Expires, X-Amz-Security-Token, X-Amz-SignedHeaders.
    char *q_cur = out_url;
    size_t q_remain = cap;

    if (0 != append_literal(&q_cur, &q_remain, "X-Amz-Algorithm=AWS4-HMAC-SHA256")) goto buf_too_small;

    if (0 != append_literal(&q_cur, &q_remain, "&X-Amz-ChannelARN=")) goto buf_too_small;
    if (0 != append_uri_encoded(&q_cur, &q_remain, creds->channel_arn, strlen(creds->channel_arn))) goto enc_fail;

    if (0 != append_literal(&q_cur, &q_remain, "&X-Amz-ClientId=")) goto buf_too_small;
    if (0 != append_uri_encoded(&q_cur, &q_remain, creds->client_id, strlen(creds->client_id))) goto enc_fail;

    // X-Amz-Credential = "<AKID>/<YYYYMMDD>/<region>/kinesisvideo/aws4_request"
    if (0 != append_literal(&q_cur, &q_remain, "&X-Amz-Credential=")) goto buf_too_small;
    {
        // Build the credential scope string in a small scratch (~120 chars max:
        // AKID 32 + 8-char date + region ~16 + service 12 + suffix 13 + 4 slashes).
        char cred_scope[160];
        int n = snprintf(cred_scope, sizeof(cred_scope),
            "%s/%.8s/%s/" KVS_SERVICE "/aws4_request",
            creds->access_key_id, date_iso, creds->region
        );
        if (n <= 0 || (size_t) n >= sizeof(cred_scope)) {
            printf("[signaling] credential scope overflow\n");
            return -1;
        }
        if (0 != append_uri_encoded(&q_cur, &q_remain, cred_scope, (size_t) n)) goto enc_fail;
    }

    if (0 != append_literal(&q_cur, &q_remain, "&X-Amz-Date=")) goto buf_too_small;
    if (0 != append_uri_encoded(&q_cur, &q_remain, date_iso, strlen(date_iso))) goto enc_fail;

    // 604800 s = 7 days, the AWS-documented max for presigned URLs. Long-lived
    // by design — the WSS connection itself outlives any single signaling
    // exchange and we don't want a 1-h URL signature expiring mid-session.
    if (0 != append_literal(&q_cur, &q_remain, "&X-Amz-Expires=604800")) goto buf_too_small;

    if (0 != append_literal(&q_cur, &q_remain, "&X-Amz-Security-Token=")) goto buf_too_small;
    if (0 != append_uri_encoded(&q_cur, &q_remain, creds->session_token, strlen(creds->session_token))) goto enc_fail;

    if (0 != append_literal(&q_cur, &q_remain, "&X-Amz-SignedHeaders=host")) goto buf_too_small;

    size_t query_len = (size_t)(q_cur - out_url);

    // 5. Build canonical headers. We sign only "host" — same as the reference
    //    port. AWS allows minimal signing for presigned URLs.
    char canon_hdr[64 + 1];
    int hdr_len = snprintf(canon_hdr, sizeof(canon_hdr), "host:%.*s\n", (int) host_len, host_start);
    if (hdr_len <= 0 || (size_t) hdr_len >= sizeof(canon_hdr)) {
        printf("[signaling] canon_hdr overflow (host_len=%d)\n", (int) host_len);
        return -1;
    }

    // 6. Sign with SigV4. QUERY_IS_CANONICAL tells SigV4 to take our query
    //    bytes verbatim (don't re-canonicalize), HEADERS_ARE_CANONICAL same
    //    for headers. IS_PRESIGNED_URL makes SigV4 use the literal string
    //    "UNSIGNED-PAYLOAD" as the payload hash — required for presigned URLs.
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
        .pHttpMethod   = "GET",
        .httpMethodLen = 3,
        .flags         = SIGV4_HTTP_PATH_IS_CANONICAL_FLAG
                       | SIGV4_HTTP_QUERY_IS_CANONICAL_FLAG
                       | SIGV4_HTTP_HEADERS_ARE_CANONICAL_FLAG
                       | SIGV4_HTTP_IS_PRESIGNED_URL,
        .pPath      = "/",
        .pathLen    = 1,
        .pQuery     = out_url,
        .queryLen   = query_len,
        .pHeaders   = canon_hdr,
        .headersLen = (size_t) hdr_len,
        .pPayload   = NULL,
        .payloadLen = 0,
    };
    SigV4Parameters_t sigv4_params = {
        .pCredentials     = &sigv4_creds,
        .pDateIso8601     = date_iso,
        .pAlgorithm       = NULL,
        .algorithmLen     = 0,
        .pRegion          = creds->region,
        .regionLen        = strlen(creds->region),
        .pService         = KVS_SERVICE,
        .serviceLen       = KVS_SERVICE_LEN,
        .pCryptoInterface = &crypto,
        .pHttpParameters  = &http_params,
    };
    size_t auth_buf_len = SIG_AUTH_BUF;
    char *sig_ptr = NULL;
    size_t sig_len = 0;
    SigV4Status_t sv4_rc = SigV4_GenerateHTTPAuthorization(
        &sigv4_params, s_auth_buf, &auth_buf_len, &sig_ptr, &sig_len
    );
    if (SigV4Success != sv4_rc) {
        printf("[signaling] SigV4_GenerateHTTPAuthorization failed: %d\n", (int) sv4_rc);
        return -1;
    }

    // 7. Append "&X-Amz-Signature=<sig>" to the query (sig is already hex-encoded).
    if (0 != append_literal(&q_cur, &q_remain, "&X-Amz-Signature=")) goto buf_too_small;
    if (sig_len >= q_remain) goto buf_too_small;
    memcpy(q_cur, sig_ptr, sig_len);
    q_cur += sig_len;
    q_remain -= sig_len;
    query_len = (size_t)(q_cur - out_url);

    // 8. Compose final URL: shift the query right to make room for
    //    "wss://<host>/?" prefix, then write the prefix at the start.
    //    Prefix length = strlen("wss://") + host_len + strlen("/?") = 8 + host_len.
    size_t prefix_len = 8 + host_len;
    if (query_len + prefix_len + 1 > cap) {
        goto buf_too_small;
    }
    memmove(out_url + prefix_len, out_url, query_len);
    memcpy(out_url, "wss://", 6);
    memcpy(out_url + 6, host_start, host_len);
    out_url[6 + host_len]     = '/';
    out_url[6 + host_len + 1] = '?';
    out_url[prefix_len + query_len] = '\0';

    printf("[signaling] presigned viewer URL ready (%d bytes)\n", (int)(prefix_len + query_len));
    return 0;

buf_too_small:
    printf("[signaling] presigned URL buffer too small (cap=%d)\n", (int) cap);
    return -1;
enc_fail:
    printf("[signaling] URI-encoding failed building presigned URL\n");
    return -1;
}

// -------------------------------------------------------------------------
// Step 5b stubs — WSS handshake + message exchange (next session)
// -------------------------------------------------------------------------

struct SignalingCtx {
    int dummy;
};

static struct SignalingCtx g_sig;

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
