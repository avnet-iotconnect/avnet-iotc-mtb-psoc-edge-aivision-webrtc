/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

/* Peer connection orchestrator.
 *
 * Increment D2 (current): peer_connection_build_answer() — assemble an SDP
 * answer body using the amazon-kinesis-video-streams-sdp serializer. Inputs:
 * the offer (existence-checked, full parse deferred), the D1 DTLS fingerprint,
 * fresh ICE ufrag/pwd from webrtc_csprng. Output: a well-formed SDP body whose
 * required lines (v=, o=, s=, t=, m=, fingerprint, ice-ufrag/pwd, setup, mid)
 * are present so Chrome would accept it past setRemoteDescription.
 *
 * D4b adds peer_connection_extract_remote_ice_creds() — a minimal scrape of
 * the offer's a=ice-ufrag / a=ice-pwd lines (not a full SDP parser). The
 * peer_connection_run / PeerConnection_WriteFrame stubs at the bottom remain
 * placeholders for D5+ (DTLS handshake, SRTP keying, RTP send loop). */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdp_serializer.h"

#include "csprng.h"
#include "dtls_transport.h"
#include "peer_connection.h"

// Buffer caps. The fingerprint string is "sha-256 " + 32*3 - 1 = 103 chars + NUL.
// ICE ufrag minimum is 4 chars (RFC 8839); 8 is comfortable. ICE pwd minimum is
// 22 chars; 24 lands on a base64 boundary cleanly. Both are URL-safe alnum so
// the SDP attribute layer doesn't need to escape.
#define PC_FP_CAP            112U
#define PC_ICE_UFRAG_LEN     8U
#define PC_ICE_PWD_LEN       24U

// alnum charset — 62 entries. Sampling from 256 truncates to multiples of 62
// without bias by rejection-sampling: the rejection probability per byte is
// 256 - (62 * 4) = 8 / 256 ≈ 3.1%, low enough to ignore for our few-byte fills.
static const char PC_ALNUM[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";


// Fill out with len alnum characters drawn from the CSPRNG. Rejection-sampled
// so each character is uniformly distributed across 62 outcomes. Overprovisions
// the random read by 25% to keep the loop short on average. Returns 0 on
// success.
static int draw_alnum(char *out, size_t len) {
    size_t produced = 0;
    while (produced < len) {
        uint8_t scratch[32];
        size_t take = (len - produced) + (len - produced) / 4 + 4;
        if (take > sizeof(scratch)) {
            take = sizeof(scratch);
        }
        if (0 != webrtc_csprng_bytes(scratch, take)) {
            printf("[pc] csprng for ufrag/pwd failed\n");
            return -1;
        }
        for (size_t i = 0; i < take && produced < len; i++) {
            uint8_t b = scratch[i];
            if (b < 62U * 4U) {
                out[produced++] = PC_ALNUM[b % 62U];
            }
        }
    }
    return 0;
}


// Add an attribute line "a=name:value" (or "a=name" if value is NULL/empty).
// Wraps the per-call error print so the call sites stay tight.
static int add_attr(SdpSerializerContext_t *ctx, const char *name, const char *value, const char *step) {
    SdpAttribute_t attr;
    attr.pAttributeName = name;
    attr.attributeNameLength = strlen(name);
    if (NULL == value || '\0' == value[0]) {
        attr.pAttributeValue = NULL;
        attr.attributeValueLength = 0;
    } else {
        attr.pAttributeValue = value;
        attr.attributeValueLength = strlen(value);
    }
    SdpResult_t rc = SdpSerializer_AddAttribute(ctx, SDP_TYPE_ATTRIBUTE, &attr);
    if (SDP_RESULT_OK != rc) {
        printf("[pc] %s failed: 0x%08x\n", step, (unsigned) rc);
        return -1;
    }
    return 0;
}


int peer_connection_build_answer(
    DtlsTransportHandle dt,
    const char *offer,
    size_t offer_len,
    const char *local_ip,
    uint16_t local_port,
    char *out,
    size_t cap,
    size_t *out_len,
    PeerConnectionLocalIceCreds *out_ice_creds
) {
    if (NULL == dt || NULL == offer || 0U == offer_len || NULL == local_ip ||
        NULL == out || NULL == out_len || cap < 256U) {
        printf("[pc] build_answer bad args\n");
        return -1;
    }

    // Pull the D1 fingerprint up front. If this fails we can't produce an
    // answer Chrome would accept, so bail before we spend ICE entropy.
    char fingerprint[PC_FP_CAP];
    if (0 != dtls_transport_get_local_fingerprint(dt, fingerprint, sizeof(fingerprint))) {
        printf("[pc] dtls_transport_get_local_fingerprint failed\n");
        return -1;
    }

    // Fresh ICE ufrag/pwd per session. KVS bursts ICE candidates from the
    // viewer keyed off these; reusing across sessions would let a stale viewer
    // poison a new one.
    char ufrag[PC_ICE_UFRAG_LEN + 1];
    char pwd[PC_ICE_PWD_LEN + 1];
    if (0 != draw_alnum(ufrag, PC_ICE_UFRAG_LEN)) {
        return -1;
    }
    ufrag[PC_ICE_UFRAG_LEN] = '\0';
    if (0 != draw_alnum(pwd, PC_ICE_PWD_LEN)) {
        return -1;
    }
    pwd[PC_ICE_PWD_LEN] = '\0';

    // Random session id for o= line. 32-bit is enough — newlib-nano's printf
    // family does not support %llu / %lld, and the SDP serializer's
    // AddOriginator path uses SDP_PRINT_FMT_UINT64 ("llu") against a uint64_t
    // arg, which crashes inside snprintf on this build. So we sidestep
    // AddOriginator entirely (hand-build the o= line below via AddBuffer)
    // and only generate as much randomness as fits in 32 bits.
    uint32_t session_id = 0;
    if (0 != webrtc_csprng_bytes((uint8_t *) &session_id, sizeof(session_id))) {
        printf("[pc] csprng for session_id failed\n");
        return -1;
    }
    // RFC 4566 says the session id is a "numeric string"; some peers parse it
    // as a signed integer. Clear the high bit to dodge negative-number paths.
    session_id &= 0x7FFFFFFFU;

    SdpSerializerContext_t sctx;
    SdpResult_t rc = SdpSerializer_Init(&sctx, out, cap);
    if (SDP_RESULT_OK != rc) {
        printf("[pc] SdpSerializer_Init failed: 0x%08x\n", (unsigned) rc);
        return -1;
    }

    // v=0
    rc = SdpSerializer_AddU32(&sctx, SDP_TYPE_VERSION, 0U);
    if (SDP_RESULT_OK != rc) {
        printf("[pc] add v= failed: 0x%08x\n", (unsigned) rc);
        return -1;
    }

    // o=- <session_id> 2 IN IP4 <local_ip>
    // Device's local IP. Hand-built via AddBuffer because AddOriginator uses
    // %llu (newlib-nano crashes on that).
    char origin_line[64];
    int n = snprintf(
        origin_line, sizeof(origin_line),
        "- %u 2 IN IP4 %s", (unsigned) session_id, local_ip
    );
    if (n <= 0 || (size_t) n >= sizeof(origin_line)) {
        printf("[pc] format o= line failed: n=%d\n", n);
        return -1;
    }
    rc = SdpSerializer_AddBuffer(&sctx, SDP_TYPE_ORIGINATOR, origin_line, (size_t) n);
    if (SDP_RESULT_OK != rc) {
        printf("[pc] add o= failed: 0x%08x\n", (unsigned) rc);
        return -1;
    }

    // s=-
    static const char SESSION_NAME_DASH[] = "-";
    rc = SdpSerializer_AddBuffer(&sctx, SDP_TYPE_SESSION_NAME, SESSION_NAME_DASH, 1);
    if (SDP_RESULT_OK != rc) {
        printf("[pc] add s= failed: 0x%08x\n", (unsigned) rc);
        return -1;
    }

    // t=0 0  (hand-built — AddTimeActive also uses %llu)
    static const char TIME_ZERO_ZERO[] = "0 0";
    rc = SdpSerializer_AddBuffer(&sctx, SDP_TYPE_TIME_ACTIVE, TIME_ZERO_ZERO, sizeof(TIME_ZERO_ZERO) - 1);
    if (SDP_RESULT_OK != rc) {
        printf("[pc] add t= failed: 0x%08x\n", (unsigned) rc);
        return -1;
    }

    // Session-level attributes. group:BUNDLE 0 1 — both m-lines under one transport.
    // msid-semantic:WMS * — declares the MSID grouping but doesn't enumerate
    // tracks; KVS / Chrome accept this short form.
    if (0 != add_attr(&sctx, "group", "BUNDLE 0 1", "add a=group")) { return -1; }
    if (0 != add_attr(&sctx, "msid-semantic", " WMS *", "add a=msid-semantic")) { return -1; }

    // m=video <local_port> UDP/TLS/RTP/SAVPF 96
    // Real UDP port device is bound to.
    SdpMedia_t media;
    static const char MEDIA_VIDEO[] = "video";
    static const char MEDIA_PROTO[] = "UDP/TLS/RTP/SAVPF";
    static const char MEDIA_FMT[] = "96";
    media.pMedia = MEDIA_VIDEO;
    media.mediaLength = sizeof(MEDIA_VIDEO) - 1;
    media.port = (uint32_t) local_port;
    media.portNum = 0U;
    media.pProtocol = MEDIA_PROTO;
    media.protocolLength = sizeof(MEDIA_PROTO) - 1;
    media.pFmt = MEDIA_FMT;
    media.fmtLength = sizeof(MEDIA_FMT) - 1;
    rc = SdpSerializer_AddMedia(&sctx, SDP_TYPE_MEDIA, &media);
    if (SDP_RESULT_OK != rc) {
        printf("[pc] add m=video failed: 0x%08x\n", (unsigned) rc);
        return -1;
    }

    // c=IN IP4 <local_ip> — device's local address for initial connectivity attempt.
    SdpConnectionInfo_t conn;
    conn.networkType = SDP_NETWORK_IN;
    conn.addressType = SDP_ADDRESS_IPV4;
    conn.pAddress = local_ip;
    conn.addressLength = strlen(local_ip);
    rc = SdpSerializer_AddConnectionInfo(&sctx, SDP_TYPE_CONNINFO, &conn);
    if (SDP_RESULT_OK != rc) {
        printf("[pc] add c= (video) failed: 0x%08x\n", (unsigned) rc);
        return -1;
    }

    // Media-level attributes for video. Order roughly mirrors Chrome's own answers.
    // - rtcp:9 IN IP4 0.0.0.0 — RTCP mux uses the same port; placeholder addr.
    // - ice-ufrag / ice-pwd / ice-options:trickle — ICE creds and trickle.
    // - fingerprint — DTLS-SRTP cert fingerprint from D1.
    // - setup:active — we initiate the DTLS handshake (offer is actpass by
    //   convention from Chrome's recvonly viewer; we pick active).
    // - mid:0 — matches BUNDLE.
    // - sendonly — device sends video to viewer (we're MASTER serving media).
    // - rtcp-mux / rtcp-rsize — single port for RTP+RTCP, reduced-size RTCP.
    // - rtpmap:96 H264/90000 — payload type binding.
    // - fmtp:96 H264 params — packetization-mode=1 (NAL+FU-A), constrained
    //   baseline level 3.1 (42e01f), level-asymmetry-allowed.
    if (0 != add_attr(&sctx, "rtcp", "9 IN IP4 0.0.0.0", "add a=rtcp (video)")) { return -1; }
    if (0 != add_attr(&sctx, "ice-ufrag", ufrag, "add a=ice-ufrag (video)")) { return -1; }
    if (0 != add_attr(&sctx, "ice-pwd", pwd, "add a=ice-pwd (video)")) { return -1; }
    if (0 != add_attr(&sctx, "ice-options", "trickle", "add a=ice-options (video)")) { return -1; }
    if (0 != add_attr(&sctx, "fingerprint", fingerprint, "add a=fingerprint (video)")) { return -1; }
    if (0 != add_attr(&sctx, "setup", "active", "add a=setup (video)")) { return -1; }
    if (0 != add_attr(&sctx, "mid", "0", "add a=mid (video)")) { return -1; }
    if (0 != add_attr(&sctx, "sendonly", NULL, "add a=sendonly (video)")) { return -1; }
    if (0 != add_attr(&sctx, "rtcp-mux", NULL, "add a=rtcp-mux (video)")) { return -1; }
    if (0 != add_attr(&sctx, "rtcp-rsize", NULL, "add a=rtcp-rsize (video)")) { return -1; }
    if (0 != add_attr(&sctx, "rtpmap", "96 H264/90000", "add a=rtpmap (video)")) { return -1; }
    if (0 != add_attr(&sctx, "fmtp", "96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f", "add a=fmtp (video)")) { return -1; }

    // m=audio 0 UDP/TLS/RTP/SAVPF 111 — rejected (port 0), but part of BUNDLE.
    // No microphone on the device, so we reject the audio m-line entirely.
    // BUNDLE still groups it, so Chrome knows to use the single transport (ICE/DTLS)
    // from the video m-line.
    static const char MEDIA_AUDIO[] = "audio";
    static const char MEDIA_FMT_AUDIO[] = "111";
    media.pMedia = MEDIA_AUDIO;
    media.mediaLength = sizeof(MEDIA_AUDIO) - 1;
    media.port = 0U;
    media.portNum = 0U;
    media.pProtocol = MEDIA_PROTO;
    media.protocolLength = sizeof(MEDIA_PROTO) - 1;
    media.pFmt = MEDIA_FMT_AUDIO;
    media.fmtLength = sizeof(MEDIA_FMT_AUDIO) - 1;
    rc = SdpSerializer_AddMedia(&sctx, SDP_TYPE_MEDIA, &media);
    if (SDP_RESULT_OK != rc) {
        printf("[pc] add m=audio failed: 0x%08x\n", (unsigned) rc);
        return -1;
    }

    // c= for audio (required by SDP structure, even though audio is rejected).
    rc = SdpSerializer_AddConnectionInfo(&sctx, SDP_TYPE_CONNINFO, &conn);
    if (SDP_RESULT_OK != rc) {
        printf("[pc] add c= (audio) failed: 0x%08x\n", (unsigned) rc);
        return -1;
    }

    // Audio m-line attributes: minimal set because BUNDLE means this m-line
    // reuses the transport (ICE/DTLS) from the video m-line. Only mid + codec info.
    if (0 != add_attr(&sctx, "mid", "1", "add a=mid (audio)")) { return -1; }
    if (0 != add_attr(&sctx, "rtpmap", "111 opus/48000/2", "add a=rtpmap (audio)")) { return -1; }

    const char *finalized = NULL;
    size_t finalized_len = 0;
    rc = SdpSerializer_Finalize(&sctx, &finalized, &finalized_len);
    if (SDP_RESULT_OK != rc) {
        printf("[pc] SdpSerializer_Finalize failed: 0x%08x\n", (unsigned) rc);
        return -1;
    }

    // The serializer wrote into our caller-supplied buffer; finalized points
    // back at it. Sanity-check and report.
    if (finalized != out || finalized_len > cap) {
        printf("[pc] serializer returned unexpected pointer/length\n");
        return -1;
    }
    *out_len = finalized_len;

    // Surface the local ICE creds so the ICE controller can use the same bytes
    // as the STUN message-integrity key. Browser signs its connectivity checks
    // with our SDP ufrag/pwd; mismatch breaks the handshake.
    if (NULL != out_ice_creds) {
        memcpy(out_ice_creds->ufrag, ufrag, PC_ICE_UFRAG_LEN);
        out_ice_creds->ufrag[PC_ICE_UFRAG_LEN] = '\0';
        out_ice_creds->ufrag_len = PC_ICE_UFRAG_LEN;
        memcpy(out_ice_creds->pwd, pwd, PC_ICE_PWD_LEN);
        out_ice_creds->pwd[PC_ICE_PWD_LEN] = '\0';
        out_ice_creds->pwd_len = PC_ICE_PWD_LEN;
    }

    // Quick UART eyeball — single line, not the body. The verification harness
    // will dump the body separately. Length acknowledges the offer was seen.
    printf(
        "[pc] answer built: %u bytes (offer was %u bytes)\n",
        (unsigned) finalized_len, (unsigned) offer_len
    );

    // Debug: log the answer SDP for inspection (will be removed once D4c is green).
    printf("[pc] answer SDP:\r\n%.*s\r\n[pc] end answer\n", (int) finalized_len, finalized);

    return 0;
}


// Find an a=<name>:<value> attribute line in the SDP starting from offset.
// Returns a pointer to the value start and writes its length (up to CR/LF) into
// out_len. NULL if not found. Matches the first occurrence — the session-level
// and m=-level ufrag/pwd are identical in Chrome's offer today, and we have
// one m-line anyway.
static const char *find_sdp_attr(const char *sdp, size_t sdp_len, const char *name, size_t *out_len) {
    size_t name_len = strlen(name);
    // Each line: "a=<name>:<value>". We search for "\na=<name>:" so we anchor
    // at the start of a line (or "a=<name>:" at offset 0).
    for (size_t i = 0; i + 2 + name_len + 1 <= sdp_len; i++) {
        bool at_line_start = (i == 0) || (sdp[i - 1] == '\n');
        if (!at_line_start) {
            continue;
        }
        if (sdp[i] != 'a' || sdp[i + 1] != '=') {
            continue;
        }
        if (0 != memcmp(sdp + i + 2, name, name_len)) {
            continue;
        }
        size_t after_name = i + 2 + name_len;
        if (after_name >= sdp_len || sdp[after_name] != ':') {
            continue;
        }
        size_t v = after_name + 1;
        size_t e = v;
        while (e < sdp_len && sdp[e] != '\r' && sdp[e] != '\n') {
            e++;
        }
        *out_len = e - v;
        return sdp + v;
    }
    return NULL;
}

int peer_connection_extract_remote_ice_creds(
    const char *offer,
    size_t offer_len,
    PeerConnectionRemoteIceCreds *out
) {
    if (NULL == offer || 0U == offer_len || NULL == out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    size_t ufrag_len = 0;
    const char *ufrag = find_sdp_attr(offer, offer_len, "ice-ufrag", &ufrag_len);
    if (NULL == ufrag) {
        printf("[pc] a=ice-ufrag not found in offer\n");
        return -1;
    }
    if (0U == ufrag_len || ufrag_len >= sizeof(out->ufrag)) {
        printf("[pc] a=ice-ufrag length out of range: %u\n", (unsigned) ufrag_len);
        return -1;
    }

    size_t pwd_len = 0;
    const char *pwd = find_sdp_attr(offer, offer_len, "ice-pwd", &pwd_len);
    if (NULL == pwd) {
        printf("[pc] a=ice-pwd not found in offer\n");
        return -1;
    }
    if (0U == pwd_len || pwd_len >= sizeof(out->pwd)) {
        printf("[pc] a=ice-pwd length out of range: %u\n", (unsigned) pwd_len);
        return -1;
    }

    memcpy(out->ufrag, ufrag, ufrag_len);
    out->ufrag[ufrag_len] = '\0';
    out->ufrag_len = ufrag_len;
    memcpy(out->pwd, pwd, pwd_len);
    out->pwd[pwd_len] = '\0';
    out->pwd_len = pwd_len;
    return 0;
}


/* -------------------------------------------------------------------------- */
/* Stubs below — replaced in later increments.                                */
/* -------------------------------------------------------------------------- */

struct PeerConnectionCtx {
    int dummy;
};

static struct PeerConnectionCtx g_pc;

PeerConnectionHandle peer_connection_create(void) {
    printf("peer_connection_create: STUB\n");
    return &g_pc;
}

void peer_connection_destroy(PeerConnectionHandle pc) {
    (void) pc;
    printf("peer_connection_destroy: STUB\n");
}

int peer_connection_run(PeerConnectionHandle pc, SignalingHandle sig) {
    (void) pc;
    (void) sig;
    printf("peer_connection_run: STUB (no real session)\n");
    return 0;
}

int PeerConnection_WriteFrame(
    PeerConnectionHandle pc,
    const unsigned char *nal,
    size_t nal_len,
    unsigned long pts_ms,
    int is_idr
) {
    (void) pc;
    (void) nal;
    (void) pts_ms;
    /* Avoid newlib-nano %z hazard: cast size_t to int. */
    printf("PeerConnection_WriteFrame: STUB len=%d idr=%d\n", (int) nal_len, is_idr);
    return 0;
}
