/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef WEBRTC_ICE_CONTROLLER_H_
#define WEBRTC_ICE_CONTROLLER_H_

#include <stddef.h>
#include <stdint.h>

#include "signaling.h"

/* lwIP sockaddr forward-declare so callers that don't pull <lwip/sockets.h>
 * still compile against this header. ice_controller.c includes lwIP itself. */
struct sockaddr;

/* Forward-declare so the nominated-pair getter has a return type without
 * forcing every TU that includes this header to drag in ice_data_types.h.
 * Pair fields are opaque to consumers — only the .c file dereferences. */
typedef struct IceCandidatePair IceCandidatePair_t;

/* ICE controller. PILOT.md §3.1 D4 architecture: single-viewer, single
 * UDP socket, file-scope state, no per-session malloc. STUN-only (region-
 * templated) — no TURN, no IPv6, no TCP candidates.
 *
 * D4b extends D4a's host-only path: real ICE creds plumbed from the SDP,
 * remote ufrag/pwd from the offer, STUN srflx via Ice_AddServerReflexiveCandidate
 * + Ice_CreateNextCandidateRequest + sendto, remote-candidate trickle-in. The
 * connectivity-check loop + pair nomination land in D4c.
 *
 * Lifecycle inside webrtc_task / run_session:
 *   ice_controller_init(udp_fd, region, local_ufrag/pwd, remote_ufrag/pwd)
 *   ice_controller_gather_host_candidates(sig)   // trickle host(s) out
 *   ice_controller_add_stun_server()             // DNS + Ice_AddServerReflexive
 *                                                // (must run after gather —
 *                                                //  the srflx is based on the
 *                                                //  host candidate's endpoint)
 *   loop:
 *     ice_controller_send_pending_requests(sig)  // STUN binding requests + trickle srflx
 *     ice_controller_handle_udp_packet(...)      // route incoming STUN
 *   ice_controller_deinit()
 */


/* Bind the controller to the shared UDP socket, AWS region, and the ICE creds
 * the SDP answer published / the offer announced. The ICE library borrows the
 * cred pointers across the session, so the controller copies them onto its
 * file-scope state — callers can drop their copies after the call returns.
 *
 * udp_fd must already be opened+bound by dtls_transport_open_socket().
 * region is borrowed for the duration of init only (copied internally).
 * Returns 0 on success. */
int ice_controller_init(
    int udp_fd,
    const char *region,
    const uint8_t *local_ufrag, size_t local_ufrag_len,
    const uint8_t *local_pwd,   size_t local_pwd_len,
    const uint8_t *remote_ufrag, size_t remote_ufrag_len,
    const uint8_t *remote_pwd,   size_t remote_pwd_len
);

/* DNS-resolve stun.kinesisvideo.<region>.amazonaws.com:443 via lwIP
 * gethostbyname and call Ice_AddServerReflexiveCandidate. The actual STUN
 * binding-request bytes are emitted later by ice_controller_send_pending_requests.
 * Returns 0 on success. */
int ice_controller_add_stun_server(void);

/* Walk the lwIP netif list, drop loopback / down / IPv6 entries, register the
 * remaining IPv4 addresses with the ICE engine via Ice_AddHostCandidate, and
 * trickle each one out over the WSS via signaling_send_ice_candidate. Returns
 * 0 on success (at least one host candidate emitted). */
int ice_controller_gather_host_candidates(SignalingHandle sig);

/* Walk local candidates with a pending request (today: srflx waiting for its
 * binding response). For each, call Ice_CreateNextCandidateRequest, sendto on
 * the shared UDP fd to the candidate's STUN server endpoint. No-op once all
 * srflx have resolved. Returns 0 on success. */
int ice_controller_send_pending_requests(void);

/* Walk candidate pairs with pending work (connectivity checks, nominations).
 * For each, call Ice_CreateNextPairRequest and sendto on the shared UDP fd
 * to pRemoteCandidate's endpoint. Library returns ICE_RESULT_NO_NEXT_ACTION
 * for pairs whose retransmit timer hasn't fired yet — quiet skip.
 * Returns 0 on success. */
int ice_controller_send_pending_pair_requests(void);

/* Return the nominated pair once the ICE library has selected one. NULL until
 * then. As the controlled side, nomination happens inside Ice_HandleStunPacket
 * when Chrome's binding request carries USE-CANDIDATE and the 4-way handshake
 * completes; the library writes pContext->pNominatedPair. This getter is the
 * exit signal the tick loop polls so run_session can hand off to D5. */
const IceCandidatePair_t *ice_controller_get_nominated_pair(void);

/* Route an incoming UDP packet (first byte 0..3 — STUN) through Ice_HandleStunPacket.
 * If the result is ICE_HANDLE_STUN_PACKET_RESULT_UPDATED_SERVER_REFLEXIVE_CANDIDATE_ADDRESS,
 * format the srflx candidate string and trickle it out via signaling_send_ice_candidate.
 * Returns 0 if the packet was handled (regardless of ICE result), -1 on hard
 * error / packet not for us. */
int ice_controller_handle_udp_packet(
    SignalingHandle sig,
    uint8_t *buf,
    size_t len,
    const struct sockaddr *from,
    int from_len
);

/* Decode an incoming ICE_CANDIDATE JSON payload (from the browser, base64-
 * decoded by signaling.c) and feed the candidate into Ice_AddRemoteCandidate.
 * IPv6 / TCP / relay / unknown types are dropped (not converted) per D4
 * architecture. Returns 0 on accept, -1 on drop or parse failure. */
int ice_controller_add_remote_candidate_json(const char *payload, size_t len);

/* Release Ice_Init resources. Does NOT close the UDP socket — that's owned
 * by dtls_transport. Idempotent. */
void ice_controller_deinit(void);

#endif /* WEBRTC_ICE_CONTROLLER_H_ */
