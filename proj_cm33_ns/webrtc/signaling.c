/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

/*
 * Signaling layer.
 *
 * Step 5a (GetSignalingChannelEndpoint, REST), Step 5b URL signing
 * (signaling_build_signed_viewer_url) and the WSS upgrade handshake
 * (signaling_connect / signaling_disconnect) are implemented. WS frame
 * exchange (wait_for_offer / send_answer) is still stubbed — wslay glue
 * is the next session.
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

#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"

#include "sigv4.h"
#include "signaling_api.h"

#include "iotcl_certs.h"
#include "iotcl_dra_url.h"
#include "iotc_http_client.h"

#include "cy_tcpip_port_secure_sockets.h"
#include "transport_interface.h"

#include "wslay/wslay.h"

#include "webrtc/csprng.h"
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

    // 5. Build the headers in plain HTTP form ("host: <host>\r\n") and let
    //    SigV4 do the canonicalization itself. The HEADERS_ARE_CANONICAL path
    //    requires the caller to also emit the empty-line separator between
    //    canonical headers and signed-headers in the canonical request — the
    //    SigV4 lib doesn't add it when the flag is set. Dropping the flag
    //    sidesteps that footgun and matches the working KVS reference.
    char hdr_buf[80];
    int hdr_len = snprintf(hdr_buf, sizeof(hdr_buf), "host: %.*s\r\n", (int) host_len, host_start);
    if (hdr_len <= 0 || (size_t) hdr_len >= sizeof(hdr_buf)) {
        printf("[signaling] hdr_buf overflow (host_len=%d)\n", (int) host_len);
        return -1;
    }

    // 6. Sign with SigV4. QUERY_IS_CANONICAL tells SigV4 to take our query
    //    bytes verbatim (don't re-canonicalize). We deliberately do NOT set
    //    IS_PRESIGNED_URL: AWS KVS-WebRTC accepts standard SigV4 presigned
    //    URLs where the payload hash is SHA256("") (empty payload) rather
    //    than the "UNSIGNED-PAYLOAD" literal — this matches the working KVS
    //    reference port. Setting IS_PRESIGNED_URL forced "UNSIGNED-PAYLOAD"
    //    into the canonical request, which produced a signature AWS rejected
    //    with HTTP 403.
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
        .flags         = SIGV4_HTTP_QUERY_IS_CANONICAL_FLAG,
        .pPath      = "/",
        .pathLen    = 1,
        .pQuery     = out_url,
        .queryLen   = query_len,
        .pHeaders   = hdr_buf,
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
// Step 5b — WSS handshake (HTTP/1.1 Upgrade)
// -------------------------------------------------------------------------
//
// We hand-roll the upgrade request and response parse rather than dragging in
// coreHTTP. The wire shape is small and well-defined; see RFC 6455 §1.3 / §4.
// After the 101 response is verified, the next session hands the still-open
// NetworkContext_t to wslay for frame I/O.
//
// Today's signaling_connect() opens the TLS connection, runs the upgrade,
// verifies Sec-WebSocket-Accept, then tears down. This proves the transport
// path end-to-end ahead of wiring wslay.

#define WSS_PORT                    443
#define WSS_RFC6455_GUID            "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WSS_NONCE_LEN               16
#define WSS_NONCE_B64_LEN           25  /* base64(16) = 24 + null */
#define WSS_ACCEPT_B64_LEN          29  /* base64(SHA1=20) = 28 + null */

// Per-call socket timeouts (ms). These are set once at cy_awsport_network_connect
// time and apply to every send/recv after that — the secure-sockets API has no
// way to retune them later. A short recv timeout is what lets the wslay event
// loop poll without blocking forever; a 0 from cy_awsport_network_receive maps
// cleanly to WSLAY_ERR_WOULDBLOCK so the loop can yield and retry.
//
// The handshake recv has to gather a multi-part response, so it loops up to
// WSS_HANDSHAKE_BUDGET_MS of wall-clock time, swallowing per-call timeouts.
#define WSS_SOCK_SEND_TIMEOUT_MS    2000U
#define WSS_SOCK_RECV_TIMEOUT_MS    200U
#define WSS_HANDSHAKE_BUDGET_MS     5000U

// Wall-clock budget for one signaling_wait_for_offer call. KVS viewer sessions
// are master-initiated, so we expect to sit idle most of the time; the budget
// here is just "how long does one call hold the caller before returning idle".
// Owner cadence for Increment B: prove the event loop runs cleanly across a
// few of these idle rotations. 10 s is short enough to keep run_session()
// responsive to creds_dirty / shutdown checks.
#define WSS_OFFER_WAIT_BUDGET_MS    10000U

// Fixed overhead in the upgrade request: method/version/literal headers/CRLFs.
// "GET  HTTP/1.1\r\n" + Host: \r\n + Upgrade: websocket\r\n + Connection: Upgrade\r\n
// + Sec-WebSocket-Key: <24>\r\n + Sec-WebSocket-Version: 13\r\n + \r\n. ~150 bytes;
// 256 leaves headroom for accidental drift in the header set.
#define WSS_REQ_FIXED_OVERHEAD      256

// Response head only — we read until "\r\n\r\n" then stop. AWS' 101 response
// is ~200 bytes (a handful of headers); 1 KB is generous.
#define WSS_RESP_BUF_LEN            1024


// Bumps to non-zero whenever the wslay recv-callback observes a hard error on
// the underlying socket (return < 0). signaling_wait_for_offer treats this as
// fatal; everything else (timeout, no message yet) is just "keep polling".
struct SignalingCtx {
    NetworkContext_t net_ctx;
    wslay_event_context_ptr ws_ctx;
    bool connected;
    bool transport_error;
    // Latched copy of the most recent received text-frame payload. The WS
    // on_msg_recv callback fires in the middle of wslay_event_recv() with a
    // buffer owned by the library, so we malloc-copy it into the handle for
    // the caller of signaling_wait_for_offer. NULL until a frame arrives;
    // freed on the next wait_for_offer call or in signaling_disconnect.
    char *latched_msg;
    size_t latched_msg_len;
};

static struct SignalingCtx g_sig;

// Split a "wss://<host>/<path-and-query>" URL into a malloc'd host and a
// path pointer (into the original buffer, after the host). On success the
// caller must free *out_host. Returns 0 on success, -1 on malformed URL.
static int wss_url_split(const char *url, char **out_host, const char **out_path) {
    if (0 != strncmp(url, "wss://", 6)) {
        printf("[signaling] not a wss:// URL\n");
        return -1;
    }
    const char *host_start = url + 6;
    const char *path_start = strchr(host_start, '/');
    if (NULL == path_start) {
        printf("[signaling] wss URL missing path\n");
        return -1;
    }
    size_t host_len = (size_t)(path_start - host_start);
    if (0 == host_len) {
        printf("[signaling] wss URL has empty host\n");
        return -1;
    }
    char *host = malloc(host_len + 1);
    if (NULL == host) {
        return -1;
    }
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    *out_host = host;
    *out_path = path_start;
    return 0;
}

// Compute the expected Sec-WebSocket-Accept value: base64(SHA1(client_key + GUID)).
// out must be at least WSS_ACCEPT_B64_LEN. Returns 0 on success.
static int wss_compute_accept(const char *client_key_b64, char *out, size_t out_size) {
    unsigned char concat[WSS_NONCE_B64_LEN - 1 + sizeof(WSS_RFC6455_GUID) - 1];
    size_t key_len = strlen(client_key_b64);
    if (key_len + sizeof(WSS_RFC6455_GUID) - 1 > sizeof(concat)) {
        return -1;
    }
    memcpy(concat, client_key_b64, key_len);
    memcpy(concat + key_len, WSS_RFC6455_GUID, sizeof(WSS_RFC6455_GUID) - 1);

    unsigned char sha1[20];
    if (0 != mbedtls_sha1(concat, key_len + sizeof(WSS_RFC6455_GUID) - 1, sha1)) {
        return -1;
    }
    size_t out_len = 0;
    if (0 != mbedtls_base64_encode((unsigned char *) out, out_size, &out_len, sha1, sizeof(sha1))) {
        return -1;
    }
    return 0;
}

// Read response head into resp_buf until "\r\n\r\n" or buffer full / wall-clock
// budget exceeded. The socket recv timeout is short (so the steady-state event
// loop can poll), so the handshake has to gather across multiple zero-returns.
// Returns the number of bytes read on success, -1 on error. The buffer is NOT
// null-terminated by this function; caller does that using the returned length.
static int wss_recv_head(NetworkContext_t *net, uint8_t *resp_buf, size_t resp_cap) {
    size_t total = 0;
    TickType_t start = xTaskGetTickCount();
    const TickType_t budget = pdMS_TO_TICKS(WSS_HANDSHAKE_BUDGET_MS);

    while (total < resp_cap) {
        if ((xTaskGetTickCount() - start) >= budget) {
            printf("[signaling] WSS upgrade recv timeout (budget %u ms)\n",
                   (unsigned) WSS_HANDSHAKE_BUDGET_MS);
            return -1;
        }
        int32_t n = cy_awsport_network_receive(net, resp_buf + total, resp_cap - total);
        if (n < 0) {
            printf("[signaling] cy_awsport_network_receive failed: %d\n", (int) n);
            return -1;
        }
        if (0 == n) {
            // Per-call timeout — keep polling until the wall-clock budget runs out.
            continue;
        }
        total += (size_t) n;
        if (total >= 4) {
            for (size_t i = 0; i + 3 < total; i++) {
                if (resp_buf[i]   == '\r' && resp_buf[i+1] == '\n'
                 && resp_buf[i+2] == '\r' && resp_buf[i+3] == '\n') {
                    return (int) total;
                }
            }
        }
    }
    printf("[signaling] WSS upgrade response head exceeded %d bytes\n", (int) resp_cap);
    return -1;
}

// Find a header line "<name>:" in the response (case-insensitive) and return a
// pointer to the value (skipping the colon and any leading whitespace), with
// *out_value_len set to the value length up to the line's CRLF. Returns NULL
// if not found. resp must point at the start of headers (after status line).
static const char *wss_find_header(const char *resp, size_t resp_len, const char *name, size_t *out_value_len) {
    size_t name_len = strlen(name);
    for (size_t i = 0; i + name_len < resp_len; ) {
        // Match "<name>:" case-insensitively at start of line.
        bool match = true;
        for (size_t j = 0; j < name_len; j++) {
            char a = resp[i + j];
            char b = name[j];
            // ASCII tolower
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) { match = false; break; }
        }
        if (match && i + name_len < resp_len && resp[i + name_len] == ':') {
            // Skip colon + LWS.
            size_t v = i + name_len + 1;
            while (v < resp_len && (resp[v] == ' ' || resp[v] == '\t')) v++;
            // Find CRLF.
            size_t e = v;
            while (e + 1 < resp_len && !(resp[e] == '\r' && resp[e+1] == '\n')) e++;
            *out_value_len = e - v;
            return resp + v;
        }
        // Advance to next line.
        while (i + 1 < resp_len && !(resp[i] == '\r' && resp[i+1] == '\n')) i++;
        i += 2;
    }
    return NULL;
}

// -------------------------------------------------------------------------
// wslay event callbacks
// -------------------------------------------------------------------------
//
// All four callbacks share a single user_data pointer (a SignalingHandle).
// Recv/send wrap cy_awsport_network_*; genmask uses our DRBG; on_msg_recv
// latches the most recent text-frame payload into the handle for
// signaling_wait_for_offer to pick up.

static ssize_t wslay_recv_cb(wslay_event_context_ptr ctx, uint8_t *buf, size_t len,
                             int flags, void *user_data) {
    (void) flags;
    SignalingHandle sig = (SignalingHandle) user_data;
    int32_t n = cy_awsport_network_receive(&sig->net_ctx, buf, len);
    if (n < 0) {
        sig->transport_error = true;
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        printf("[signaling] wslay recv: socket error %d\n", (int) n);
        return -1;
    }
    if (0 == n) {
        // Per-call socket timeout — wslay treats WOULDBLOCK as "stop receiving
        // for this tick, try again later".
        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }
    return (ssize_t) n;
}

static ssize_t wslay_send_cb(wslay_event_context_ptr ctx, const uint8_t *data, size_t len,
                             int flags, void *user_data) {
    (void) flags;
    SignalingHandle sig = (SignalingHandle) user_data;
    int32_t n = cy_awsport_network_send(&sig->net_ctx, data, len);
    if (n < 0) {
        sig->transport_error = true;
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        printf("[signaling] wslay send: socket error %d\n", (int) n);
        return -1;
    }
    if (0 == n) {
        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }
    return (ssize_t) n;
}

static int wslay_genmask_cb(wslay_event_context_ptr ctx, uint8_t *buf, size_t len,
                            void *user_data) {
    (void) ctx;
    (void) user_data;
    if (0 != webrtc_csprng_bytes(buf, len)) {
        return -1;
    }
    return 0;
}

static void wslay_on_msg_recv_cb(wslay_event_context_ptr ctx,
                                 const struct wslay_event_on_msg_recv_arg *arg,
                                 void *user_data) {
    (void) ctx;
    SignalingHandle sig = (SignalingHandle) user_data;

    if (wslay_is_ctrl_frame(arg->opcode)) {
        // wslay handles ping/close/pong queueing itself; just log for triage.
        if (WSLAY_CONNECTION_CLOSE == arg->opcode) {
            printf("[signaling] WS close (status=%u, len=%u)\n",
                   (unsigned) arg->status_code, (unsigned) arg->msg_length);
        }
        return;
    }

    // Replace any prior un-consumed message — KVS sends one envelope per text
    // frame, and signaling_wait_for_offer is expected to drain promptly.
    free(sig->latched_msg);
    sig->latched_msg = NULL;
    sig->latched_msg_len = 0;

    char *copy = malloc(arg->msg_length + 1);
    if (NULL == copy) {
        printf("[signaling] WS msg: OOM (%u bytes)\n", (unsigned) arg->msg_length);
        return;
    }
    memcpy(copy, arg->msg, arg->msg_length);
    copy[arg->msg_length] = '\0';
    sig->latched_msg = copy;
    sig->latched_msg_len = arg->msg_length;
    printf("[signaling] WS msg: opcode=0x%x len=%u\n",
           (unsigned) arg->opcode, (unsigned) arg->msg_length);
}

static const struct wslay_event_callbacks g_wslay_cbs = {
    wslay_recv_cb,
    wslay_send_cb,
    wslay_genmask_cb,
    NULL,                   // on_frame_recv_start
    NULL,                   // on_frame_recv_chunk
    NULL,                   // on_frame_recv_end
    wslay_on_msg_recv_cb,
};


SignalingHandle signaling_connect(const char *signed_url) {
    if (NULL == signed_url) {
        return NULL;
    }
    if (g_sig.connected) {
        printf("[signaling] signaling_connect called while already connected\n");
        return NULL;
    }

    char *host = NULL;
    const char *path = NULL;
    if (0 != wss_url_split(signed_url, &host, &path)) {
        return NULL;
    }

    // 1. Generate Sec-WebSocket-Key: 16 random bytes, base64-encoded.
    uint8_t nonce[WSS_NONCE_LEN];
    if (0 != webrtc_csprng_bytes(nonce, sizeof(nonce))) {
        printf("[signaling] csprng failed for Sec-WebSocket-Key\n");
        free(host);
        return NULL;
    }
    char nonce_b64[WSS_NONCE_B64_LEN];
    size_t nonce_b64_len = 0;
    if (0 != mbedtls_base64_encode((unsigned char *) nonce_b64, sizeof(nonce_b64), &nonce_b64_len, nonce, sizeof(nonce))) {
        printf("[signaling] base64 encode failed for Sec-WebSocket-Key\n");
        free(host);
        return NULL;
    }
    nonce_b64[nonce_b64_len] = '\0';

    // 2. Open TLS connection to host:443. Cert verify against AmazonRootCA1.
    cy_awsport_server_info_t server_info = {
        .host_name = host,
        .port      = WSS_PORT,
    };
    cy_awsport_ssl_credentials_t ssl = {
        .root_ca           = (const char *) IOTCL_AMAZON_ROOT_CA1,
        .root_ca_size      = strlen(IOTCL_AMAZON_ROOT_CA1) + 1,
        .root_ca_verify_mode = CY_AWS_ROOTCA_VERIFY_REQUIRED,
        .sni_host_name     = host,
        .sni_host_name_size = strlen(host) + 1,
    };

    cy_rslt_t r = cy_awsport_network_create(&g_sig.net_ctx, &server_info, &ssl, NULL, NULL);
    if (CY_RSLT_SUCCESS != r) {
        printf("[signaling] cy_awsport_network_create failed: 0x%08lx\n", (unsigned long) r);
        free(host);
        return NULL;
    }
    r = cy_awsport_network_connect(&g_sig.net_ctx, WSS_SOCK_SEND_TIMEOUT_MS, WSS_SOCK_RECV_TIMEOUT_MS);
    if (CY_RSLT_SUCCESS != r) {
        printf("[signaling] cy_awsport_network_connect failed: 0x%08lx\n", (unsigned long) r);
        cy_awsport_network_delete(&g_sig.net_ctx);
        free(host);
        return NULL;
    }

    // 3. Build the upgrade request. Size the buffer from the actual URL —
    //    presigned URLs run multi-KB once the URI-encoded session token is
    //    in there, so a fixed scratch doesn't cut it.
    size_t req_cap = strlen(path) + strlen(host) + WSS_REQ_FIXED_OVERHEAD;
    char *req = malloc(req_cap);
    if (NULL == req) {
        cy_awsport_network_disconnect(&g_sig.net_ctx);
        cy_awsport_network_delete(&g_sig.net_ctx);
        free(host);
        return NULL;
    }
    int req_len = snprintf(req, req_cap,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, nonce_b64
    );
    if (req_len <= 0 || (size_t) req_len >= req_cap) {
        printf("[signaling] WSS upgrade request did not fit in %d bytes\n", (int) req_cap);
        free(req);
        cy_awsport_network_disconnect(&g_sig.net_ctx);
        cy_awsport_network_delete(&g_sig.net_ctx);
        free(host);
        return NULL;
    }

    int32_t sent = cy_awsport_network_send(&g_sig.net_ctx, req, (size_t) req_len);
    free(req);
    if (sent != req_len) {
        printf("[signaling] WSS upgrade send short/failed: %d/%d\n", (int) sent, req_len);
        cy_awsport_network_disconnect(&g_sig.net_ctx);
        cy_awsport_network_delete(&g_sig.net_ctx);
        free(host);
        return NULL;
    }

    // 4. Receive the response head.
    uint8_t resp[WSS_RESP_BUF_LEN];
    int resp_len = wss_recv_head(&g_sig.net_ctx, resp, sizeof(resp));
    if (resp_len < 0) {
        cy_awsport_network_disconnect(&g_sig.net_ctx);
        cy_awsport_network_delete(&g_sig.net_ctx);
        free(host);
        return NULL;
    }

    // 5. Verify status line is "HTTP/1.1 101".
    //    Min length of a valid 101 line is "HTTP/1.1 101 X\r\n" — 16 bytes.
    if (resp_len < 16 || 0 != memcmp(resp, "HTTP/1.1 101", 12)) {
        // Print the status line for triage.
        size_t line_end = 0;
        while (line_end + 1 < (size_t) resp_len && !(resp[line_end] == '\r' && resp[line_end+1] == '\n')) line_end++;
        printf("[signaling] WSS upgrade not 101: %.*s\n", (int) line_end, (const char *) resp);
        cy_awsport_network_disconnect(&g_sig.net_ctx);
        cy_awsport_network_delete(&g_sig.net_ctx);
        free(host);
        return NULL;
    }

    // 6. Find Sec-WebSocket-Accept and verify it matches base64(SHA1(key+GUID)).
    //    Skip the status line so the header search starts at the first header.
    size_t hdr_off = 0;
    while (hdr_off + 1 < (size_t) resp_len && !(resp[hdr_off] == '\r' && resp[hdr_off+1] == '\n')) hdr_off++;
    hdr_off += 2;
    size_t accept_len = 0;
    const char *accept_val = wss_find_header((const char *) resp + hdr_off, (size_t) resp_len - hdr_off,
                                             "Sec-WebSocket-Accept", &accept_len);
    if (NULL == accept_val) {
        printf("[signaling] WSS upgrade response missing Sec-WebSocket-Accept\n");
        cy_awsport_network_disconnect(&g_sig.net_ctx);
        cy_awsport_network_delete(&g_sig.net_ctx);
        free(host);
        return NULL;
    }
    char expected[WSS_ACCEPT_B64_LEN];
    if (0 != wss_compute_accept(nonce_b64, expected, sizeof(expected))) {
        printf("[signaling] failed to compute expected Sec-WebSocket-Accept\n");
        cy_awsport_network_disconnect(&g_sig.net_ctx);
        cy_awsport_network_delete(&g_sig.net_ctx);
        free(host);
        return NULL;
    }
    size_t expected_len = strlen(expected);
    if (accept_len != expected_len || 0 != memcmp(accept_val, expected, expected_len)) {
        printf("[signaling] Sec-WebSocket-Accept mismatch (got %.*s, want %s)\n",
            (int) accept_len, accept_val, expected);
        cy_awsport_network_disconnect(&g_sig.net_ctx);
        cy_awsport_network_delete(&g_sig.net_ctx);
        free(host);
        return NULL;
    }

    free(host);

    // 7. 101 verified — hand the open NetworkContext_t to wslay. From here on
    //    all I/O on this connection goes through the event-loop callbacks.
    int wrc = wslay_event_context_client_init(&g_sig.ws_ctx, &g_wslay_cbs, &g_sig);
    if (0 != wrc) {
        printf("[signaling] wslay_event_context_client_init failed: %d\n", wrc);
        cy_awsport_network_disconnect(&g_sig.net_ctx);
        cy_awsport_network_delete(&g_sig.net_ctx);
        return NULL;
    }

    g_sig.connected = true;
    g_sig.transport_error = false;
    g_sig.latched_msg = NULL;
    g_sig.latched_msg_len = 0;
    printf("[signaling] WS upgrade OK (101 Switching Protocols)\n");
    return &g_sig;
}

void signaling_disconnect(SignalingHandle sig) {
    if (NULL == sig || !sig->connected) {
        return;
    }
    if (NULL != sig->ws_ctx) {
        wslay_event_context_free(sig->ws_ctx);
        sig->ws_ctx = NULL;
    }
    free(sig->latched_msg);
    sig->latched_msg = NULL;
    sig->latched_msg_len = 0;
    cy_awsport_network_disconnect(&sig->net_ctx);
    cy_awsport_network_delete(&sig->net_ctx);
    sig->connected = false;
}

// Drive the wslay event loop until either a non-control message lands (latched
// by wslay_on_msg_recv_cb) or budget_ms elapses. Returns 0 on message, 1 on
// idle timeout (no error — caller may retry), -1 on transport / protocol error.
//
// The current owner cadence for Increment B is "loop runs without faulting" —
// receiving an actual SDP offer is master-initiated and not part of the gate.
// The TODO past this point is parsing the JSON envelope (messageType /
// messagePayload, base64-decoded) into a real SDP offer string.
int signaling_wait_for_offer(SignalingHandle sig, const char **out_offer) {
    if (NULL == sig || NULL == out_offer || !sig->connected || NULL == sig->ws_ctx) {
        return -1;
    }
    *out_offer = NULL;

    // Free any prior un-consumed latched message before we start polling — the
    // caller has had its chance to read the previous one.
    free(sig->latched_msg);
    sig->latched_msg = NULL;
    sig->latched_msg_len = 0;

    const TickType_t budget = pdMS_TO_TICKS(WSS_OFFER_WAIT_BUDGET_MS);
    TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < budget) {
        // Send any queued frames first (close, pong, future messages).
        if (wslay_event_want_write(sig->ws_ctx)) {
            int wrc = wslay_event_send(sig->ws_ctx);
            if (0 != wrc) {
                printf("[signaling] wslay_event_send: %d\n", wrc);
                return -1;
            }
        }

        if (wslay_event_want_read(sig->ws_ctx)) {
            int wrc = wslay_event_recv(sig->ws_ctx);
            if (0 != wrc) {
                printf("[signaling] wslay_event_recv: %d\n", wrc);
                return -1;
            }
        } else {
            // Read side disabled (close received / shutdown_read called) and
            // nothing left to write — connection is effectively done.
            if (!wslay_event_want_write(sig->ws_ctx)) {
                printf("[signaling] WS read+write both disabled; closing\n");
                return -1;
            }
        }

        if (sig->transport_error) {
            return -1;
        }

        if (NULL != sig->latched_msg) {
            *out_offer = sig->latched_msg;
            return 0;
        }

        // Yield. The recv callback already blocks up to WSS_SOCK_RECV_TIMEOUT_MS,
        // but want_read may be 0 (e.g., right after a control-frame round trip),
        // so a small extra yield keeps this loop cooperative.
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return 1;  // idle timeout
}

int signaling_send_answer(SignalingHandle sig, const char *sdp_answer) {
    (void) sig;
    (void) sdp_answer;
    printf("[signaling] signaling_send_answer: STUB\n");
    return -1;
}
