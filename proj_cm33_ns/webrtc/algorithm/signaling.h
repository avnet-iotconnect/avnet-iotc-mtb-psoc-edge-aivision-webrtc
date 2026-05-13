/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef WEBRTC_SIGNALING_H_
#define WEBRTC_SIGNALING_H_

#include <stddef.h>

#include "peer_connection_data_types.h"
#include "webrtc/aws_creds.h"

typedef struct SignalingCtx *SignalingHandle;

/* GetSignalingChannelEndpoint: SigV4-signed HTTPS POST.
 * Returns 0 on success and writes the wss host (no scheme, no path) into
 * out_endpoint.
 *
 * out_endpoint capacity: ~256 chars is plenty (KVS hostnames are short). */
int signaling_resolve_endpoint(const AwsCreds *creds, char *out_endpoint, size_t cap);

/* Build the signed wss:// URL for ConnectAsMaster.
 *
 * KVS role naming gotcha: a camera streaming video to a browser is the
 * MASTER (sits on the channel, waits for viewers). The role names describe
 * signaling-channel topology, not media direction. */
int signaling_build_signed_url(
    const AwsCreds *creds,
    const char *wss_endpoint,
    char *out_url,
    size_t cap
);

/* WSS upgrade against the signed URL (wslay over secure-sockets TLS). */
SignalingHandle signaling_connect(const char *signed_url);
void signaling_disconnect(SignalingHandle sig);

/* Bind or clear the peer-connection session that inbound ICE_CANDIDATE frames
 * should feed into. The pointer is borrowed; caller owns the session lifetime. */
void signaling_set_peer_connection(SignalingHandle sig, PeerConnectionSession_t *session);

/* Block until a viewer publishes an SDP offer (or the socket dies). On
 * success returns 0, writes the decoded SDP into out_sdp (null-terminated),
 * and latches the viewer's senderClientId on the handle so the matching
 * signaling_send_answer routes the reply back to the right viewer. Returns
 * -1 on transport error, peer close, malformed envelope, or non-SDP_OFFER
 * message (ICE_CANDIDATE / GO_AWAY / etc. land here later). */
int signaling_wait_for_offer(SignalingHandle sig, char *out_sdp, size_t out_sdp_cap, size_t *out_sdp_len);

/* Wrap an SDP answer in the KVS WSS send envelope (action=SDP_ANSWER,
 * RecipientClientId=latched senderClientId, MessagePayload=base64(sdp))
 * and queue it for the next signaling_tick drain. */
int signaling_send_answer(SignalingHandle sig, const char *sdp_answer);

/* Non-blocking pump of the wslay event loop — drains any queued send frames
 * and reads any pending recv frames (which fan out into dispatch_text_frame /
 * the ICE-controller path). Intended to be called once per tick from the
 * D4b webrtc_task tick loop. Returns 0 on success, -1 on transport error or
 * peer close (caller should tear down the session). */
int signaling_tick(SignalingHandle sig);

/* Trickle-out a single ICE candidate. Wraps it in the KVS WSS send envelope
 * (action=ICE_CANDIDATE, RecipientClientId=latched senderClientId,
 * MessagePayload=base64({"candidate":...,"sdpMid":...,"sdpMLineIndex":N})).
 * Candidate is the SDP "candidate:..." body (no "a=" prefix). The frame is
 * queued and drained by signaling_tick. */
int signaling_send_ice_candidate(
    SignalingHandle sig,
    const char *candidate,
    const char *sdp_mid,
    int sdp_m_line_index
);

#endif /* WEBRTC_SIGNALING_H_ */
