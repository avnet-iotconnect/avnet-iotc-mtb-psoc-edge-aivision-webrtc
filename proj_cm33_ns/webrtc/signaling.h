/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef WEBRTC_SIGNALING_H_
#define WEBRTC_SIGNALING_H_

#include <stddef.h>
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

/* Block until a viewer publishes an SDP offer (or the socket dies). On
 * success returns 0, writes the decoded SDP into out_sdp (null-terminated),
 * and latches the viewer's senderClientId on the handle so the matching
 * signaling_send_answer routes the reply back to the right viewer. Returns
 * -1 on transport error, peer close, malformed envelope, or non-SDP_OFFER
 * message (ICE_CANDIDATE / GO_AWAY / etc. land here later). */
int signaling_wait_for_offer(SignalingHandle sig, char *out_sdp, size_t out_sdp_cap, size_t *out_sdp_len);

/* Wrap an SDP answer in the KVS WSS send envelope (action=SDP_ANSWER,
 * RecipientClientId=latched senderClientId, MessagePayload=base64(sdp))
 * and push it through wslay. Drains wslay_event_send before returning so
 * the caller can observe send errors. */
int signaling_send_answer(SignalingHandle sig, const char *sdp_answer);

#endif /* WEBRTC_SIGNALING_H_ */
