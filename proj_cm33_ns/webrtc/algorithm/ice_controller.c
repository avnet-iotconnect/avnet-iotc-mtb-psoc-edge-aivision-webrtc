/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * ICE controller — D4b slice. Adds STUN srflx + remote-candidate trickle-in
 * on top of D4a's host gather. Connectivity-check loop + pair nomination land
 * in D4c.
 *
 * State is at file scope (single-viewer assumption baked in per PILOT.md
 * §3.1 D4 architecture). Pre-allocated arrays sized to the documented caps:
 * 4 local candidates (host + srflx, with headroom), 8 remote (typical Chrome
 * count), 12 pairs (cross product cap). No per-session malloc.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "mbedtls/md.h"
#include "mbedtls/md5.h"

/* lwIP headers — see dtls_transport.c note about the timeval squiggle. */
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"

#include "ice_api.h"
#include "ice_data_types.h"
#include "transaction_id_store.h"

#include "csprng.h"
#include "ice_controller.h"
#include "signaling.h"

/* Pair table cap from PILOT §3.1: typical Chrome browser ships ~6 remote
 * candidates, we have 1 host + 1 srflx local → cross product fits in 12. */
#define ICE_LOCAL_CANDIDATE_MAX     ( 4U )
#define ICE_REMOTE_CANDIDATE_MAX    ( 8U )
#define ICE_PAIR_MAX                ( 12U )

/* SDP candidate string: "candidate:<f> 1 udp <prio> <ip> <port> typ <type>" + the
 * trailer the reference uses. ~120 B is plenty (IPv4 dotted-quad max 15 chars,
 * priority max 10 digits, port max 5 digits, srflx adds "raddr/rport"). */
#define ICE_CANDIDATE_STR_BUF       ( 192U )

/* STUN scratch — outgoing binding requests. Per GUIDELINES "Buffer allocation":
 * fixed, small, single-stage transient, owning task's stack budget covers it.
 * Bare STUN binding request lives in ~200 B; 512 leaves headroom for any
 * future TURN-style attrs we accidentally generate. */
#define ICE_STUN_TX_BUF             ( 512U )

/* Region buffer cap. AWS region codes are bounded at 50 chars by API. */
#define ICE_REGION_MAX              ( 64U )

/* Local creds: ufrag and pwd sizes pulled from peer_connection — 8 + NUL and
 * 24 + NUL today, with the bigger remote buffers to absorb whatever Chrome
 * sends (RFC 8839 ufrag/pwd upper bound is 256). */
#define ICE_LOCAL_UFRAG_MAX         ( 16U )
#define ICE_LOCAL_PWD_MAX           ( 32U )
#define ICE_REMOTE_UFRAG_MAX        ( 64U )
#define ICE_REMOTE_PWD_MAX          ( 128U )
/* combined = local_ufrag + ":" + remote_ufrag + NUL = 16+1+64+1 = 82. Round up. */
#define ICE_COMBINED_USER_MAX       ( 96U )


typedef struct IceController {
    bool initialized;
    int udp_fd;
    char region[ICE_REGION_MAX];
    char stun_host[128];   /* "stun.kinesisvideo.<region>.amazonaws.com" */
    IceContext_t ctx;
    IceCandidate_t local_candidates[ICE_LOCAL_CANDIDATE_MAX];
    IceCandidate_t remote_candidates[ICE_REMOTE_CANDIDATE_MAX];
    IceCandidatePair_t pairs[ICE_PAIR_MAX];
    IceTurnServer_t turn_unused[1];     /* Ice_Init validates non-NULL. */
    TransactionIdStore_t tx_id_store;
    TransactionIdSlot_t tx_id_slots[ICE_PAIR_MAX];
    /* ICE creds. The library borrows these pointers across the session, so
     * they live on this struct and are valid until deinit. The combined
     * username is "<remote-ufrag>:<local-ufrag>" per RFC 8445 §7.2.2 (the
     * remote's ufrag goes first because the STUN message-integrity key is
     * the remote's password). */
    uint8_t local_ufrag[ICE_LOCAL_UFRAG_MAX];
    uint8_t local_pwd[ICE_LOCAL_PWD_MAX];
    uint8_t remote_ufrag[ICE_REMOTE_UFRAG_MAX];
    uint8_t remote_pwd[ICE_REMOTE_PWD_MAX];
    uint8_t combined_user[ICE_COMBINED_USER_MAX];
    size_t local_ufrag_len;
    size_t local_pwd_len;
    size_t remote_ufrag_len;
    size_t remote_pwd_len;
    size_t combined_user_len;
    /* STUN server endpoint resolved from DNS. Owned by a srflx-typed local
     * candidate via Ice_AddServerReflexiveCandidate. */
    IceEndpoint_t stun_endpoint;
    bool stun_endpoint_resolved;
    /* Foundation counter for the SDP candidate string's first field. */
    uint16_t foundation;
    /* Has the srflx candidate already been trickled out? Set once
     * Ice_HandleStunPacket reports UPDATED_SERVER_REFLEXIVE_CANDIDATE_ADDRESS. */
    bool srflx_trickled;
} IceController_t;

static IceController_t g_ice;

/* Pre-init holding pen for remote ICE candidates. Chrome trickles candidates
 * the moment the offer is sent — they arrive at signaling.c before
 * ice_controller_init runs (build_answer + send_answer happen in between).
 * Pre-init candidates are stashed here as the raw decoded JSON payload and
 * replayed through ice_controller_add_remote_candidate_json once init
 * succeeds. Lives outside g_ice so the memset in _init doesn't wipe it.
 *
 * KVS / the browser may re-trickle the same candidate ~10x while waiting for
 * our answer to be accepted; we don't dedupe here — Ice_AddRemoteCandidate
 * rejects repeats on drain. The cap is sized for that retry burst plus a
 * handful of distinct candidates, with a verbose log if it overflows. */
#define ICE_PENDING_REMOTE_MAX        ( 16U )
#define ICE_PENDING_REMOTE_PAYLOAD    ( 256U )

typedef struct PendingRemoteCandidate {
    char payload[ICE_PENDING_REMOTE_PAYLOAD];
    size_t len;
} PendingRemoteCandidate_t;

static PendingRemoteCandidate_t g_pending_remote[ICE_PENDING_REMOTE_MAX];
static size_t g_pending_remote_count;
static size_t g_pending_remote_overflow;


/* --------------------------------------------------------------------- */
/* ICE crypto callbacks — minimal real impls.                            */
/* --------------------------------------------------------------------- */

static IceResult_t ice_random_fxn(uint8_t *out, size_t len) {
    if (0 != webrtc_csprng_bytes(out, len)) {
        return ICE_RESULT_RANDOM_GENERATION_ERROR;
    }
    return ICE_RESULT_OK;
}

/* IEEE 802.3 CRC32 — STUN FINGERPRINT attribute (RFC 5389). Reflected,
 * polynomial 0xEDB88320. Bitwise version; no table — fingerprint is computed
 * once per outgoing STUN request, not per packet. */
static IceResult_t ice_crc32_fxn(uint32_t initial, const uint8_t *buf, size_t len, uint32_t *out) {
    uint32_t c = initial ^ 0xFFFFFFFFU;
    if (NULL != buf) {
        for (size_t i = 0; i < len; i++) {
            c ^= buf[i];
            for (int b = 0; b < 8; b++) {
                c = (c >> 1) ^ (0xEDB88320U & -(c & 1U));
            }
        }
    }
    *out = c ^ 0xFFFFFFFFU;
    return ICE_RESULT_OK;
}

static IceResult_t ice_hmac_fxn(const uint8_t *key, size_t key_len,
                                const uint8_t *buf, size_t buf_len,
                                uint8_t *out, uint16_t *out_len) {
    int rc = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
                             key, key_len, buf, buf_len, out);
    if (0 != rc) {
        return ICE_RESULT_HMAC_ERROR;
    }
    *out_len = 20U;
    return ICE_RESULT_OK;
}

static IceResult_t ice_md5_fxn(const uint8_t *buf, size_t buf_len,
                               uint8_t *out, uint16_t *out_len) {
    if (*out_len < 16U) {
        return ICE_RESULT_MD5_ERROR;
    }
    if (0 != mbedtls_md5(buf, buf_len, out)) {
        return ICE_RESULT_MD5_ERROR;
    }
    *out_len = 16U;
    return ICE_RESULT_OK;
}


/* --------------------------------------------------------------------- */
/* Internal helpers                                                      */
/* --------------------------------------------------------------------- */

static int copy_bounded(uint8_t *dst, size_t dst_cap,
                        const uint8_t *src, size_t src_len) {
    if (NULL == src || 0U == src_len || src_len >= dst_cap) {
        return -1;
    }
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
    return 0;
}

/* Build "<remote-ufrag>:<local-ufrag>" — RFC 8445 §7.2.2 USERNAME attribute
 * for outgoing connectivity checks. We're the controlled (viewer) side per
 * Ice_Init's isControlling=0; library uses this for STUN message integrity. */
static int build_combined_username(void) {
    if (g_ice.remote_ufrag_len + 1U + g_ice.local_ufrag_len >= sizeof(g_ice.combined_user)) {
        return -1;
    }
    memcpy(g_ice.combined_user, g_ice.remote_ufrag, g_ice.remote_ufrag_len);
    g_ice.combined_user[g_ice.remote_ufrag_len] = ':';
    memcpy(g_ice.combined_user + g_ice.remote_ufrag_len + 1U,
           g_ice.local_ufrag, g_ice.local_ufrag_len);
    g_ice.combined_user_len = g_ice.remote_ufrag_len + 1U + g_ice.local_ufrag_len;
    g_ice.combined_user[g_ice.combined_user_len] = '\0';
    return 0;
}

/* Build the SDP candidate body (no "a=" prefix) for a candidate we own. */
static int format_local_candidate_string(
    char *out, size_t cap,
    uint16_t foundation, uint32_t priority,
    const char *type_name,
    const uint8_t addr[4], uint16_t port,
    bool include_raddr,
    const uint8_t raddr[4], uint16_t rport
) {
    int n;
    if (include_raddr) {
        n = snprintf(out, cap,
            "candidate:%u 1 udp %u %u.%u.%u.%u %u typ %s "
            "raddr %u.%u.%u.%u rport %u generation 0",
            (unsigned) foundation,
            (unsigned) priority,
            (unsigned) addr[0], (unsigned) addr[1],
            (unsigned) addr[2], (unsigned) addr[3],
            (unsigned) port,
            type_name,
            (unsigned) raddr[0], (unsigned) raddr[1],
            (unsigned) raddr[2], (unsigned) raddr[3],
            (unsigned) rport);
    } else {
        n = snprintf(out, cap,
            "candidate:%u 1 udp %u %u.%u.%u.%u %u typ %s generation 0",
            (unsigned) foundation,
            (unsigned) priority,
            (unsigned) addr[0], (unsigned) addr[1],
            (unsigned) addr[2], (unsigned) addr[3],
            (unsigned) port,
            type_name);
    }
    if (n <= 0 || (size_t) n >= cap) {
        return -1;
    }
    return n;
}

/* Cheap monotonic seconds — Ice_CreateNextCandidateRequest / Ice_HandleStunPacket
 * want a uint64_t for currentTimeSeconds. FreeRTOS tick rate is 1 kHz on this
 * build; xTaskGetTickCount() is a 32-bit free-running counter, so dividing by
 * configTICK_RATE_HZ gives seconds for ~49.7 days before wraparound. The ICE
 * library uses this only for retransmit pacing within a session, so wraparound
 * during a session is not a concern. */
static uint64_t current_time_seconds(void) {
    return (uint64_t) xTaskGetTickCount() / (uint64_t) configTICK_RATE_HZ;
}

/* sendto wrapper that converts an IceEndpoint_t IPv4 address to sockaddr_in
 * and pushes the STUN bytes onto the shared UDP fd. Returns 0 on success. */
static int send_stun_to_endpoint(const IceEndpoint_t *ep, const uint8_t *buf, size_t len) {
    if (STUN_ADDRESS_IPv4 != ep->transportAddress.family) {
        return -1;
    }
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(ep->transportAddress.port);
    memcpy(&to.sin_addr.s_addr, ep->transportAddress.address, 4);
    int sent = sendto(g_ice.udp_fd, buf, len, 0,
                      (struct sockaddr *) &to, sizeof(to));
    if (sent < 0 || (size_t) sent != len) {
        printf("[ice] sendto failed (sent=%d, len=%u)\n", sent, (unsigned) len);
        return -1;
    }
    return 0;
}

/* Trickle the srflx candidate out over the signaling channel. Called once
 * Ice_HandleStunPacket reports UPDATED_SERVER_REFLEXIVE_CANDIDATE_ADDRESS for
 * the srflx local candidate the library now has populated. */
static int trickle_srflx_candidate(SignalingHandle sig, const IceCandidate_t *cand) {
    /* Pull the host-base IP so the raddr/rport carry the right backing
     * address — Chrome expects srflx's raddr/rport to be the local host pair
     * the candidate was discovered from. We have at most one host candidate
     * today (single wlan netif); use the first one. */
    const uint8_t *raddr_bytes = NULL;
    uint16_t raddr_port = 0;
    for (size_t i = 0; i < g_ice.ctx.numLocalCandidates; i++) {
        const IceCandidate_t *c = &g_ice.ctx.pLocalCandidates[i];
        if (ICE_CANDIDATE_TYPE_HOST == c->candidateType
            && STUN_ADDRESS_IPv4 == c->endpoint.transportAddress.family) {
            raddr_bytes = c->endpoint.transportAddress.address;
            raddr_port = c->endpoint.transportAddress.port;
            break;
        }
    }
    bool include_raddr = (NULL != raddr_bytes);

    char cand_str[ICE_CANDIDATE_STR_BUF];
    uint8_t dummy_raddr[4] = {0};
    int n = format_local_candidate_string(
        cand_str, sizeof(cand_str),
        g_ice.foundation, cand->priority, "srflx",
        cand->endpoint.transportAddress.address,
        cand->endpoint.transportAddress.port,
        include_raddr,
        include_raddr ? raddr_bytes : dummy_raddr,
        include_raddr ? raddr_port  : 0
    );
    if (n < 0) {
        printf("[ice] srflx candidate string overflow\n");
        return -1;
    }
    printf("[ice] srflx candidate: %s\n", cand_str);
    int rc = signaling_send_ice_candidate(sig, cand_str, "0", 0);
    if (0 != rc) {
        printf("[ice] signaling_send_ice_candidate (srflx) failed\n");
        return -1;
    }
    g_ice.foundation++;
    return 0;
}


/* --------------------------------------------------------------------- */
/* Public API                                                            */
/* --------------------------------------------------------------------- */

int ice_controller_init(
    int udp_fd,
    const char *region,
    const uint8_t *local_ufrag, size_t local_ufrag_len,
    const uint8_t *local_pwd,   size_t local_pwd_len,
    const uint8_t *remote_ufrag, size_t remote_ufrag_len,
    const uint8_t *remote_pwd,   size_t remote_pwd_len
) {
    if (udp_fd < 0 || NULL == region) {
        return -1;
    }
    if (g_ice.initialized) {
        printf("[ice] init called while already initialized\n");
        return -1;
    }

    memset(&g_ice, 0, sizeof(g_ice));
    g_ice.udp_fd = udp_fd;
    size_t region_len = strlen(region);
    if (region_len >= sizeof(g_ice.region)) {
        printf("[ice] region too long (%u >= %u)\n",
               (unsigned) region_len, (unsigned) sizeof(g_ice.region));
        return -1;
    }
    memcpy(g_ice.region, region, region_len + 1);

    if (0 != copy_bounded(g_ice.local_ufrag, sizeof(g_ice.local_ufrag), local_ufrag, local_ufrag_len)
     || 0 != copy_bounded(g_ice.local_pwd,   sizeof(g_ice.local_pwd),   local_pwd,   local_pwd_len)
     || 0 != copy_bounded(g_ice.remote_ufrag, sizeof(g_ice.remote_ufrag), remote_ufrag, remote_ufrag_len)
     || 0 != copy_bounded(g_ice.remote_pwd,   sizeof(g_ice.remote_pwd),   remote_pwd,   remote_pwd_len)) {
        printf("[ice] cred bounds violated (lu=%u lp=%u ru=%u rp=%u)\n",
               (unsigned) local_ufrag_len, (unsigned) local_pwd_len,
               (unsigned) remote_ufrag_len, (unsigned) remote_pwd_len);
        return -1;
    }
    g_ice.local_ufrag_len  = local_ufrag_len;
    g_ice.local_pwd_len    = local_pwd_len;
    g_ice.remote_ufrag_len = remote_ufrag_len;
    g_ice.remote_pwd_len   = remote_pwd_len;
    if (0 != build_combined_username()) {
        printf("[ice] combined username overflow\n");
        return -1;
    }

    TransactionIdStore_Init(&g_ice.tx_id_store, g_ice.tx_id_slots, ICE_PAIR_MAX);

    IceInitInfo_t info = {
        .creds = {
            .pLocalUsername          = g_ice.local_ufrag,
            .localUsernameLength     = g_ice.local_ufrag_len,
            .pLocalPassword          = g_ice.local_pwd,
            .localPasswordLength     = g_ice.local_pwd_len,
            .pRemoteUsername         = g_ice.remote_ufrag,
            .remoteUsernameLength    = g_ice.remote_ufrag_len,
            .pRemotePassword         = g_ice.remote_pwd,
            .remotePasswordLength    = g_ice.remote_pwd_len,
            .pCombinedUsername       = g_ice.combined_user,
            .combinedUsernameLength  = g_ice.combined_user_len,
        },
        .pLocalCandidatesArray             = g_ice.local_candidates,
        .localCandidatesArrayLength        = ICE_LOCAL_CANDIDATE_MAX,
        .pRemoteCandidatesArray            = g_ice.remote_candidates,
        .remoteCandidatesArrayLength       = ICE_REMOTE_CANDIDATE_MAX,
        .pCandidatePairsArray              = g_ice.pairs,
        .candidatePairsArrayLength         = ICE_PAIR_MAX,
        .pTurnServerArray                  = g_ice.turn_unused,
        .turnServerArrayLength             = 0,
        .isControlling                     = 0,                 /* viewer is controlled. */
        .pStunBindingRequestTransactionIdStore = &g_ice.tx_id_store,
        .cryptoFunctions = {
            .randomFxn = ice_random_fxn,
            .crc32Fxn  = ice_crc32_fxn,
            .hmacFxn   = ice_hmac_fxn,
            .md5Fxn    = ice_md5_fxn,
        },
    };

    IceResult_t r = Ice_Init(&g_ice.ctx, &info);
    if (ICE_RESULT_OK != r) {
        printf("[ice] Ice_Init failed: %d\n", (int) r);
        return -1;
    }

    g_ice.initialized = true;
    printf("[ice] init OK (fd=%d region=%s lu=%u lp=%u ru=%u rp=%u)\n",
           udp_fd, g_ice.region,
           (unsigned) local_ufrag_len, (unsigned) local_pwd_len,
           (unsigned) remote_ufrag_len, (unsigned) remote_pwd_len);

    /* Drain anything signaling.c stashed before init. Ice_AddRemoteCandidate
     * filters duplicates by (protocol, ip, port, type), so the browser's
     * retry burst naturally collapses on the way through. */
    if (g_pending_remote_count > 0U) {
        printf("[ice] draining %u pre-init remote candidate(s) (overflow=%u)\n",
               (unsigned) g_pending_remote_count, (unsigned) g_pending_remote_overflow);
        for (size_t i = 0; i < g_pending_remote_count; i++) {
            (void) ice_controller_add_remote_candidate_json(
                g_pending_remote[i].payload, g_pending_remote[i].len);
        }
        g_pending_remote_count = 0U;
        g_pending_remote_overflow = 0U;
        memset(g_pending_remote, 0, sizeof(g_pending_remote));
    }
    return 0;
}


int ice_controller_add_stun_server(void) {
    if (!g_ice.initialized) {
        return -1;
    }
    int n = snprintf(g_ice.stun_host, sizeof(g_ice.stun_host),
        "stun.kinesisvideo.%s.amazonaws.com", g_ice.region);
    if (n <= 0 || (size_t) n >= sizeof(g_ice.stun_host)) {
        printf("[ice] STUN host string overflow\n");
        return -1;
    }

    /* lwIP getaddrinfo with AF_INET hint — gethostbyname returns whatever DNS
     * gave first (often the AAAA record on dual-stack builds; LWIP_IPV6=1 in
     * wifi-core's lwipopts.h). KVS STUN endpoint is IPv4 only, so we force the
     * family. */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(g_ice.stun_host, NULL, &hints, &res);
    if (0 != gai || NULL == res) {
        printf("[ice] getaddrinfo(%s, AF_INET) failed: gai=%d\n", g_ice.stun_host, gai);
        return -1;
    }
    if (AF_INET != res->ai_family || res->ai_addrlen < sizeof(struct sockaddr_in)) {
        printf("[ice] STUN host AF_INET request got family=%d len=%u\n",
               res->ai_family, (unsigned) res->ai_addrlen);
        freeaddrinfo(res);
        return -1;
    }
    const struct sockaddr_in *sin = (const struct sockaddr_in *) res->ai_addr;

    memset(&g_ice.stun_endpoint, 0, sizeof(g_ice.stun_endpoint));
    g_ice.stun_endpoint.transportAddress.family = STUN_ADDRESS_IPv4;
    g_ice.stun_endpoint.transportAddress.port = 443U;     /* AWS STUN endpoint. */
    memcpy(g_ice.stun_endpoint.transportAddress.address, &sin->sin_addr.s_addr, 4);
    g_ice.stun_endpoint_resolved = true;
    freeaddrinfo(res);

    const uint8_t *a = g_ice.stun_endpoint.transportAddress.address;
    printf("[ice] STUN endpoint resolved: %s -> %u.%u.%u.%u:%u\n",
           g_ice.stun_host,
           (unsigned) a[0], (unsigned) a[1], (unsigned) a[2], (unsigned) a[3],
           (unsigned) g_ice.stun_endpoint.transportAddress.port);

    /* Ice_AddServerReflexiveCandidate seeds the new local candidate's
     * endpoint with what we pass here. Ice_HandleServerReflexiveResponse later
     * overwrites it with the public address from the STUN binding response.
     * Pass the host-base endpoint so the candidate is identifiable in logs
     * between request and response. Requires gather_host_candidates to have
     * run first — N6 reference does the same ordering. */
    const IceEndpoint_t *base = NULL;
    for (size_t i = 0; i < g_ice.ctx.numLocalCandidates; i++) {
        if (ICE_CANDIDATE_TYPE_HOST == g_ice.ctx.pLocalCandidates[i].candidateType) {
            base = &g_ice.ctx.pLocalCandidates[i].endpoint;
            break;
        }
    }
    if (NULL == base) {
        printf("[ice] add_stun_server: no host candidate to base srflx on\n");
        return -1;
    }
    IceResult_t r = Ice_AddServerReflexiveCandidate(&g_ice.ctx, base);
    if (ICE_RESULT_OK != r) {
        printf("[ice] Ice_AddServerReflexiveCandidate failed: %d\n", (int) r);
        return -1;
    }
    return 0;
}


int ice_controller_gather_host_candidates(SignalingHandle sig) {
    if (!g_ice.initialized || NULL == sig) {
        return -1;
    }

    /* Discover the kernel-picked port from the bound UDP socket. The
     * IceEndpoint port for host candidates is the local UDP port — what the
     * remote peer will sendto() to reach us. */
    struct sockaddr_in bound;
    socklen_t bound_len = sizeof(bound);
    if (getsockname(g_ice.udp_fd, (struct sockaddr *) &bound, &bound_len) < 0) {
        printf("[ice] getsockname failed\n");
        return -1;
    }
    uint16_t local_port = ntohs(bound.sin_port);

    int emitted = 0;
    struct netif *nif;
    NETIF_FOREACH(nif) {
        if (nif->name[0] == 'l' && nif->name[1] == 'o') {
            continue;
        }
        if (!netif_is_up(nif) || !netif_is_link_up(nif)) {
            continue;
        }
        const ip4_addr_t *ip4 = netif_ip4_addr(nif);
        if (NULL == ip4 || ip4->addr == 0) {
            continue;
        }

        IceEndpoint_t ep;
        memset(&ep, 0, sizeof(ep));
        ep.transportAddress.family = STUN_ADDRESS_IPv4;
        ep.transportAddress.port   = local_port;
        memcpy(ep.transportAddress.address, &ip4->addr, 4);
        ep.isPointToPoint = 0;

        IceResult_t r = Ice_AddHostCandidate(&g_ice.ctx, &ep);
        if (ICE_RESULT_OK != r) {
            printf("[ice] Ice_AddHostCandidate failed: %d\n", (int) r);
            continue;
        }

        size_t idx = g_ice.ctx.numLocalCandidates;
        if (idx == 0) {
            continue;
        }
        IceCandidate_t *cand = &g_ice.ctx.pLocalCandidates[idx - 1];

        char cand_str[ICE_CANDIDATE_STR_BUF];
        int n = format_local_candidate_string(
            cand_str, sizeof(cand_str),
            g_ice.foundation, cand->priority, "host",
            ep.transportAddress.address, local_port,
            false, NULL, 0
        );
        if (n < 0) {
            printf("[ice] candidate string overflow\n");
            continue;
        }

        printf("[ice] host candidate: %s\n", cand_str);
        if (0 != signaling_send_ice_candidate(sig, cand_str, "0", 0)) {
            printf("[ice] signaling_send_ice_candidate failed\n");
        }
        g_ice.foundation++;
        emitted++;
    }

    if (0 == emitted) {
        printf("[ice] no host candidates emitted (no usable IPv4 netif?)\n");
        return -1;
    }
    return 0;
}


int ice_controller_send_pending_pair_requests(void) {
    if (!g_ice.initialized) {
        return -1;
    }
    uint64_t now = current_time_seconds();
    uint8_t stun_tx[ICE_STUN_TX_BUF];

    /* TEMP instrumentation — strip when D4c is green. Print pair-table size
     * only when it changes so we don't spam every tick. */
    static size_t last_num_pairs = (size_t) -1;
    if (g_ice.ctx.numCandidatePairs != last_num_pairs) {
        printf("[ice/dbg] pair table now has %u pair(s)\n",
               (unsigned) g_ice.ctx.numCandidatePairs);
        last_num_pairs = g_ice.ctx.numCandidatePairs;
    }

    for (size_t i = 0; i < g_ice.ctx.numCandidatePairs; i++) {
        IceCandidatePair_t *pair = &g_ice.ctx.pCandidatePairs[i];
        size_t buf_len = sizeof(stun_tx);

        IceResult_t r = Ice_CreateNextPairRequest(
            &g_ice.ctx, pair, now, stun_tx, &buf_len);
        if (ICE_RESULT_NO_NEXT_ACTION == r) {
            continue;
        }
        if (ICE_RESULT_OK != r) {
            printf("[ice] CreateNextPairRequest pair %u failed: %d\n",
                   (unsigned) i, (int) r);
            continue;
        }
        if (NULL == pair->pRemoteCandidate) {
            continue;
        }
        /* TEMP instrumentation — strip when D4c is green. */
        const uint8_t *a = pair->pRemoteCandidate->endpoint.transportAddress.address;
        printf("[ice/dbg] pair %u check -> %u.%u.%u.%u:%u (len=%u)\n",
               (unsigned) i, a[0], a[1], a[2], a[3],
               (unsigned) pair->pRemoteCandidate->endpoint.transportAddress.port,
               (unsigned) buf_len);
        (void) send_stun_to_endpoint(&pair->pRemoteCandidate->endpoint, stun_tx, buf_len);
    }
    return 0;
}


const IceCandidatePair_t *ice_controller_get_nominated_pair(void) {
    if (!g_ice.initialized) {
        return NULL;
    }
    return g_ice.ctx.pNominatedPair;
}


int ice_controller_send_pending_requests(void) {
    if (!g_ice.initialized) {
        return -1;
    }
    uint64_t now = current_time_seconds();
    uint8_t stun_tx[ICE_STUN_TX_BUF];

    for (size_t i = 0; i < g_ice.ctx.numLocalCandidates; i++) {
        IceCandidate_t *cand = &g_ice.ctx.pLocalCandidates[i];
        size_t buf_len = sizeof(stun_tx);

        IceResult_t r = Ice_CreateNextCandidateRequest(
            &g_ice.ctx, cand, now, stun_tx, &buf_len);
        if (ICE_RESULT_NO_NEXT_ACTION == r) {
            continue;
        }
        if (ICE_RESULT_OK != r) {
            printf("[ice] CreateNextCandidateRequest cand %u failed: %d\n",
                   (unsigned) i, (int) r);
            continue;
        }

        /* For a srflx candidate the destination is the STUN server endpoint.
         * Host candidates wouldn't have pending requests in this loop — only
         * srflx waits for a binding response in D4b. (D4c will drive
         * connectivity checks via Ice_CreateNextPairRequest, a different
         * code path.) */
        if (ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE == cand->candidateType
            && g_ice.stun_endpoint_resolved) {
            /* TEMP instrumentation — strip when D4c is green. */
            const uint8_t *a = g_ice.stun_endpoint.transportAddress.address;
            printf("[ice/dbg] srflx binding req -> %u.%u.%u.%u:%u (len=%u)\n",
                   a[0], a[1], a[2], a[3],
                   (unsigned) g_ice.stun_endpoint.transportAddress.port,
                   (unsigned) buf_len);
            if (0 != send_stun_to_endpoint(&g_ice.stun_endpoint, stun_tx, buf_len)) {
                continue;
            }
        }
    }
    return 0;
}


/* Build and send a STUN binding success response for an incoming connectivity
 * check. Called from the SEND_RESPONSE_FOR_REMOTE_REQUEST / SEND_TRIGGERED_CHECK
 * branches of the switch below. The response goes back to pRemoteCandidate's
 * endpoint, which the library populated from the recvfrom source address. */
static void send_binding_response(const IceCandidatePair_t *pair, uint8_t *txn) {
    if (NULL == pair || NULL == pair->pRemoteCandidate || NULL == txn) {
        return;
    }
    uint8_t resp[ICE_STUN_TX_BUF];
    size_t resp_len = sizeof(resp);
    IceResult_t r = Ice_CreateResponseForRequest(
        &g_ice.ctx, pair, txn, resp, &resp_len);
    if (ICE_RESULT_OK != r) {
        printf("[ice] CreateResponseForRequest failed: %d\n", (int) r);
        return;
    }
    (void) send_stun_to_endpoint(&pair->pRemoteCandidate->endpoint, resp, resp_len);
}


/* Iteration order for pLocalCandidate when handling an incoming packet. The
 * library matches the pair by both pLocalCandidate and pRemoteCandidateEndpoint
 * transport addresses; with one UDP socket we don't know from recvfrom which
 * local the remote aimed at. Walk host first (typical Chrome path on LAN),
 * then srflx (STUN response or remote-side public check). Whichever yields
 * the most informative result wins. */
static IceCandidate_t *pick_local_candidate_by_index(size_t idx) {
    size_t seen = 0;
    /* Pass 1: host candidates. */
    for (size_t i = 0; i < g_ice.ctx.numLocalCandidates; i++) {
        if (ICE_CANDIDATE_TYPE_HOST == g_ice.ctx.pLocalCandidates[i].candidateType) {
            if (seen == idx) return &g_ice.ctx.pLocalCandidates[i];
            seen++;
        }
    }
    /* Pass 2: srflx. */
    for (size_t i = 0; i < g_ice.ctx.numLocalCandidates; i++) {
        if (ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE == g_ice.ctx.pLocalCandidates[i].candidateType) {
            if (seen == idx) return &g_ice.ctx.pLocalCandidates[i];
            seen++;
        }
    }
    return NULL;
}


int ice_controller_handle_udp_packet(
    SignalingHandle sig,
    uint8_t *buf,
    size_t len,
    const struct sockaddr *from,
    int from_len
) {
    (void) from_len;
    if (!g_ice.initialized || NULL == buf || len < 4) {
        return -1;
    }

    /* Build the remote endpoint from the recvfrom() src — Ice_HandleStunPacket
     * uses this to attribute the packet to the right pair and to recognize
     * the srflx address echoed back by the STUN server. */
    IceEndpoint_t remote;
    memset(&remote, 0, sizeof(remote));
    if (NULL != from && AF_INET == ((const struct sockaddr_in *) from)->sin_family) {
        const struct sockaddr_in *fin = (const struct sockaddr_in *) from;
        remote.transportAddress.family = STUN_ADDRESS_IPv4;
        remote.transportAddress.port = ntohs(fin->sin_port);
        memcpy(remote.transportAddress.address, &fin->sin_addr.s_addr, 4);
    } else {
        return -1;
    }

    /* TEMP instrumentation — strip when D4c is green. */
    {
        const uint8_t *a = remote.transportAddress.address;
        printf("[ice/dbg] udp in: first=0x%02x len=%u from %u.%u.%u.%u:%u\n",
               (unsigned) buf[0], (unsigned) len,
               a[0], a[1], a[2], a[3], (unsigned) remote.transportAddress.port);
    }

    /* The library expects the caller to know which local candidate the
     * incoming packet belongs to (N6 has one socket per local candidate,
     * so the socket context carries pLocalCandidate). We collapsed to a
     * single UDP socket per D4 architecture, so recvfrom can't tell us.
     *
     * Peek the STUN message type (bytes [0..1], network byte order) to
     * dispatch:
     *   0x0101 BINDING_SUCCESS_RESPONSE — only source in our world is the
     *          STUN server replying to our srflx binding request. Pass srflx.
     *   0x0001 BINDING_REQUEST — Chrome's connectivity check. Library
     *          matches by pair (local + remote endpoints), not by txn-id,
     *          so the host-first retry is safe (no state corruption). Walk
     *          host then srflx until we get a non-fallthrough result.
     *
     * A previous attempt walked locals for both message types, but the
     * library mutates the srflx txn-id store on the first call: when we
     * passed host first for a SUCCESS_RESPONSE, the library matched the
     * txn-id, rejected host as wrong candidate type (rc=17), and removed
     * the id from the store anyway. The srflx retry then got rc=20
     * (PAIR_NOT_FOUND) because the id was already gone. */
    bool is_response = (len >= 2 && 0x01 == buf[0] && 0x01 == buf[1]);

    IceHandleStunPacketResult_t r = ICE_HANDLE_STUN_PACKET_RESULT_BAD_PARAM;
    IceCandidate_t *local_cand = NULL;
    uint8_t *txn = NULL;
    IceCandidatePair_t *pair = NULL;

    if (is_response) {
        /* Pass srflx directly. */
        for (size_t i = 0; i < g_ice.ctx.numLocalCandidates; i++) {
            if (ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE == g_ice.ctx.pLocalCandidates[i].candidateType) {
                local_cand = &g_ice.ctx.pLocalCandidates[i];
                break;
            }
        }
        if (NULL != local_cand) {
            r = Ice_HandleStunPacket(
                &g_ice.ctx, buf, len, local_cand, &remote,
                current_time_seconds(), &txn, &pair);
            /* TEMP instrumentation — strip when D4c is green. */
            printf("[ice/dbg] HandleStunPacket (response) local=srflx rc=%d pair=%p\n",
                   (int) r, (void *) pair);
        }
    } else {
        /* Binding request or other — walk locals. Host first matches Chrome's
         * typical LAN check; srflx fallback handles public-side checks. */
        for (size_t i = 0; ; i++) {
            local_cand = pick_local_candidate_by_index(i);
            if (NULL == local_cand) {
                break;
            }
            txn = NULL;
            pair = NULL;
            r = Ice_HandleStunPacket(
                &g_ice.ctx, buf, len, local_cand, &remote,
                current_time_seconds(), &txn, &pair);
            /* TEMP instrumentation — strip when D4c is green. */
            printf("[ice/dbg] HandleStunPacket (request) local=%s rc=%d pair=%p\n",
                   (ICE_CANDIDATE_TYPE_HOST == local_cand->candidateType) ? "host" : "srflx",
                   (int) r, (void *) pair);
            if (ICE_HANDLE_STUN_PACKET_RESULT_CANDIDATE_PAIR_NOT_FOUND != r
             && ICE_HANDLE_STUN_PACKET_RESULT_INVALID_CANDIDATE_TYPE != r) {
                break;
            }
        }
    }

    switch (r) {
        case ICE_HANDLE_STUN_PACKET_RESULT_UPDATED_SERVER_REFLEXIVE_CANDIDATE_ADDRESS:
            /* The library has now populated the srflx candidate's endpoint
             * with our public IP/port as seen by the STUN server. Trickle it
             * out. Idempotent guard — Chrome retransmits binding responses if
             * we retransmit requests; only the first updated address is news. */
            if (!g_ice.srflx_trickled && NULL != local_cand) {
                if (0 == trickle_srflx_candidate(sig, local_cand)) {
                    g_ice.srflx_trickled = true;
                }
            }
            break;

        case ICE_HANDLE_STUN_PACKET_RESULT_SEND_RESPONSE_FOR_REMOTE_REQUEST:
            /* Chrome's connectivity check. Library has matched the pair and
             * built a transaction id; we serialize the response and send it
             * back to pRemoteCandidate's endpoint (recvfrom source). */
            send_binding_response(pair, txn);
            break;

        case ICE_HANDLE_STUN_PACKET_RESULT_SEND_TRIGGERED_CHECK:
            /* Two-step per library docs (ice_api_private.c:1492-1503): we owe
             * Chrome a response now, AND we need to send our own connectivity
             * check on this pair. The next tick's
             * ice_controller_send_pending_pair_requests pass picks up the
             * outgoing check via Ice_CreateNextPairRequest; do the response
             * here so it goes out without a tick delay. */
            send_binding_response(pair, txn);
            break;

        case ICE_HANDLE_STUN_PACKET_RESULT_SEND_RESPONSE_AND_START_NOMINATION:
            /* Only the controlling side actually starts nomination. We're
             * controlled; this shouldn't fire. Send the response anyway and
             * log so we see the surprise. */
            send_binding_response(pair, txn);
            printf("[ice] unexpected SEND_RESPONSE_AND_START_NOMINATION (we're controlled)\n");
            break;

        case ICE_HANDLE_STUN_PACKET_RESULT_START_NOMINATION:
            /* Controlling-side action. Logged for symmetry. */
            printf("[ice] unexpected START_NOMINATION (we're controlled)\n");
            break;

        case ICE_HANDLE_STUN_PACKET_RESULT_VALID_CANDIDATE_PAIR:
            if (NULL != pair && NULL != pair->pLocalCandidate && NULL != pair->pRemoteCandidate) {
                const uint8_t *la = pair->pLocalCandidate->endpoint.transportAddress.address;
                const uint8_t *ra = pair->pRemoteCandidate->endpoint.transportAddress.address;
                printf("[ice] pair valid: local %u.%u.%u.%u:%u <-> remote %u.%u.%u.%u:%u\n",
                       la[0], la[1], la[2], la[3],
                       (unsigned) pair->pLocalCandidate->endpoint.transportAddress.port,
                       ra[0], ra[1], ra[2], ra[3],
                       (unsigned) pair->pRemoteCandidate->endpoint.transportAddress.port);
            }
            break;

        case ICE_HANDLE_STUN_PACKET_RESULT_CANDIDATE_PAIR_READY:
            /* Pair has completed the 4-way handshake. If Chrome already sent
             * USE-CANDIDATE the library transitioned state to SUCCEEDED and
             * set pNominatedPair; the tick loop polls that and exits. Quiet
             * one-line log is enough. */
            printf("[ice] pair ready (4-way handshake complete)\n");
            break;

        case ICE_HANDLE_STUN_PACKET_RESULT_NOT_STUN_PACKET:
            /* DTLS/RTP — not our gate yet. Caller (run_session) should have
             * already first-byte-demuxed this away from us, so log if it
             * leaks through. */
            printf("[ice] non-STUN packet reached handler (first byte 0x%02x)\n", (unsigned) buf[0]);
            break;

        case ICE_HANDLE_STUN_PACKET_RESULT_OK:
        case ICE_HANDLE_STUN_PACKET_RESULT_MATCHING_TRANSACTION_ID_NOT_FOUND:
            /* Quiet success / known-uninteresting. */
            break;

        case ICE_HANDLE_STUN_PACKET_RESULT_CANDIDATE_PAIR_NOT_FOUND:
        case ICE_HANDLE_STUN_PACKET_RESULT_INVALID_CANDIDATE_TYPE:
            /* Iteration exhausted without finding the matching local
             * candidate. PAIR_NOT_FOUND fires when no pair exists yet;
             * INVALID_CANDIDATE_TYPE fires from
             * Ice_HandleServerReflexiveResponse when a srflx binding-response
             * arrives but no local was the srflx (or it already resolved
             * and txn-id store has stale entries). Quiet drop in both cases. */
            break;

        case ICE_HANDLE_STUN_PACKET_RESULT_INTEGRITY_MISMATCH:
            /* Loud — likely a cred-plumbing bug. ufrag/pwd mismatch between
             * what we put in the SDP answer and what the library uses for
             * HMAC validation. */
            printf("[ice] STUN integrity mismatch — check ICE creds plumbing\n");
            break;

        default:
            printf("[ice] Ice_HandleStunPacket rc=%d\n", (int) r);
            break;
    }
    return 0;
}


/* --------------------------------------------------------------------- */
/* Remote-candidate intake (ICE_CANDIDATE WSS frames)                    */
/* --------------------------------------------------------------------- */

/* Find the value for a JSON key in a small flat object — no nested arrays,
 * no escape handling. Sufficient for the {"candidate":"…","sdpMid":"0",
 * "sdpMLineIndex":0,"usernameFragment":"…"} shape Chrome sends. Returns a
 * pointer to the value (just past the opening quote, or at the first digit
 * for numeric values) and writes its length to out_len. */
static const char *find_json_string(const char *json, size_t len, const char *key, size_t *out_len) {
    size_t key_len = strlen(key);
    /* Look for "key":" */
    for (size_t i = 0; i + key_len + 4 <= len; i++) {
        if (json[i] != '"') {
            continue;
        }
        if (0 != memcmp(json + i + 1, key, key_len)) {
            continue;
        }
        if (json[i + 1 + key_len] != '"') {
            continue;
        }
        /* Skip past "key" then : and optional spaces */
        size_t p = i + 1 + key_len + 1;
        while (p < len && (json[p] == ':' || json[p] == ' ' || json[p] == '\t')) {
            p++;
        }
        if (p >= len || json[p] != '"') {
            return NULL;
        }
        size_t v = p + 1;
        size_t e = v;
        while (e < len && json[e] != '"') {
            e++;
        }
        *out_len = e - v;
        return json + v;
    }
    return NULL;
}

/* Parse a token from an SDP candidate body. Skips leading spaces, returns the
 * span of the next non-space token and advances *p past it. NULL on overrun. */
static const char *next_token(const char **p, const char *end, size_t *out_len) {
    while (*p < end && (**p == ' ' || **p == '\t')) {
        (*p)++;
    }
    const char *start = *p;
    while (*p < end && **p != ' ' && **p != '\t') {
        (*p)++;
    }
    if (start == *p) {
        return NULL;
    }
    *out_len = (size_t)(*p - start);
    return start;
}

/* Parse a non-negative decimal integer from [s, s+len). Returns 0 on success. */
static int parse_uint(const char *s, size_t len, uint32_t *out) {
    if (0U == len) {
        return -1;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10U + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

/* Parse "a.b.c.d" into 4 bytes. Returns 0 on success. */
static int parse_ipv4(const char *s, size_t len, uint8_t out[4]) {
    int parts = 0;
    uint32_t accum = 0;
    bool had_digit = false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            accum = accum * 10U + (uint32_t)(c - '0');
            if (accum > 255U) {
                return -1;
            }
            had_digit = true;
        } else if (c == '.') {
            if (!had_digit || parts >= 3) {
                return -1;
            }
            out[parts++] = (uint8_t) accum;
            accum = 0;
            had_digit = false;
        } else {
            return -1;
        }
    }
    if (!had_digit || parts != 3) {
        return -1;
    }
    out[3] = (uint8_t) accum;
    return 0;
}

/* Parse a single "candidate:..." SDP body. Extracts the fields we need to
 * call Ice_AddRemoteCandidate — type, priority, IPv4 address, port. Drops
 * non-IPv4 / non-UDP / unknown-type entries. */
static int parse_sdp_candidate(
    const char *cand, size_t len,
    IceRemoteCandidateInfo_t *out, IceEndpoint_t *out_ep
) {
    const char *p = cand;
    const char *end = cand + len;

    /* "candidate:<foundation>" — confirm prefix and skip past the colon. */
    static const char PREFIX[] = "candidate:";
    if (len < sizeof(PREFIX) - 1 || 0 != memcmp(cand, PREFIX, sizeof(PREFIX) - 1)) {
        return -1;
    }
    p += sizeof(PREFIX) - 1;
    /* Skip foundation digits (we don't use them). */
    while (p < end && *p != ' ' && *p != '\t') {
        p++;
    }

    size_t tok_len = 0;
    const char *tok;

    /* component id — must be 1 (we only use RTP, with rtcp-mux). */
    tok = next_token(&p, end, &tok_len);
    if (NULL == tok) return -1;
    uint32_t component = 0;
    if (0 != parse_uint(tok, tok_len, &component) || 1U != component) {
        return -1;
    }

    /* transport — UDP only. TCP candidates are dropped per D4 architecture. */
    tok = next_token(&p, end, &tok_len);
    if (NULL == tok || 3 != tok_len) return -1;
    if (0 != memcmp(tok, "udp", 3) && 0 != memcmp(tok, "UDP", 3)) {
        return -1;
    }

    /* priority */
    tok = next_token(&p, end, &tok_len);
    if (NULL == tok) return -1;
    uint32_t priority = 0;
    if (0 != parse_uint(tok, tok_len, &priority)) {
        return -1;
    }

    /* connection-address */
    tok = next_token(&p, end, &tok_len);
    if (NULL == tok) return -1;
    uint8_t addr[4];
    if (0 != parse_ipv4(tok, tok_len, addr)) {
        /* Likely IPv6 ("::1", "fe80::…") — drop per D4 architecture. */
        return -1;
    }

    /* port */
    tok = next_token(&p, end, &tok_len);
    if (NULL == tok) return -1;
    uint32_t port = 0;
    if (0 != parse_uint(tok, tok_len, &port) || port > 0xFFFFU) {
        return -1;
    }

    /* "typ" keyword */
    tok = next_token(&p, end, &tok_len);
    if (NULL == tok || 3 != tok_len || 0 != memcmp(tok, "typ", 3)) {
        return -1;
    }

    /* candidate-type */
    tok = next_token(&p, end, &tok_len);
    if (NULL == tok) return -1;
    IceCandidateType_t type;
    if (4 == tok_len && 0 == memcmp(tok, "host", 4)) {
        type = ICE_CANDIDATE_TYPE_HOST;
    } else if (5 == tok_len && 0 == memcmp(tok, "srflx", 5)) {
        type = ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE;
    } else if (5 == tok_len && 0 == memcmp(tok, "prflx", 5)) {
        type = ICE_CANDIDATE_TYPE_PEER_REFLEXIVE;
    } else {
        /* relay or unknown — drop. */
        return -1;
    }

    memset(out_ep, 0, sizeof(*out_ep));
    out_ep->transportAddress.family = STUN_ADDRESS_IPv4;
    out_ep->transportAddress.port = (uint16_t) port;
    memcpy(out_ep->transportAddress.address, addr, 4);
    out_ep->isPointToPoint = 0;

    memset(out, 0, sizeof(*out));
    out->candidateType = type;
    out->remoteProtocol = ICE_SOCKET_PROTOCOL_UDP;
    out->priority = priority;
    out->pEndpoint = out_ep;
    return 0;
}


int ice_controller_add_remote_candidate_json(const char *payload, size_t len) {
    if (NULL == payload || 0U == len) {
        return -1;
    }
    if (!g_ice.initialized) {
        /* Pre-init holding pen — see g_pending_remote declaration. */
        if (len >= ICE_PENDING_REMOTE_PAYLOAD) {
            printf("[ice] pre-init remote candidate too large (%u >= %u), dropped\n",
                   (unsigned) len, (unsigned) ICE_PENDING_REMOTE_PAYLOAD);
            return -1;
        }
        if (g_pending_remote_count >= ICE_PENDING_REMOTE_MAX) {
            g_pending_remote_overflow++;
            return -1;
        }
        PendingRemoteCandidate_t *slot = &g_pending_remote[g_pending_remote_count];
        memcpy(slot->payload, payload, len);
        slot->len = len;
        g_pending_remote_count++;
        return 0;
    }

    size_t cand_len = 0;
    const char *cand = find_json_string(payload, len, "candidate", &cand_len);
    if (NULL == cand || 0U == cand_len) {
        /* End-of-candidates frames carry an empty "candidate" — quietly skip. */
        return -1;
    }

    IceRemoteCandidateInfo_t info;
    IceEndpoint_t ep;
    if (0 != parse_sdp_candidate(cand, cand_len, &info, &ep)) {
        /* Verbose because IPv6/relay drops are expected and noisy. Single
         * line keeps it greppable. */
        printf("[ice] remote candidate dropped: %.*s\n", (int) cand_len, cand);
        return -1;
    }

    IceResult_t r = Ice_AddRemoteCandidate(&g_ice.ctx, &info);
    if (ICE_RESULT_OK != r) {
        printf("[ice] Ice_AddRemoteCandidate failed: %d (cand: %.*s)\n",
               (int) r, (int) cand_len, cand);
        return -1;
    }
    printf("[ice] remote candidate added: %.*s\n", (int) cand_len, cand);
    return 0;
}


void ice_controller_deinit(void) {
    if (!g_ice.initialized) {
        return;
    }
    /* The ICE library has no Ice_Deinit. State lives entirely in our
     * zero-able struct, so a memset on next init is the teardown. The UDP
     * socket is owned by dtls_transport — don't close it here. */
    g_ice.initialized = false;
    g_ice.udp_fd      = -1;
    g_ice.foundation  = 0;
    g_ice.srflx_trickled = false;
    g_ice.stun_endpoint_resolved = false;
}
