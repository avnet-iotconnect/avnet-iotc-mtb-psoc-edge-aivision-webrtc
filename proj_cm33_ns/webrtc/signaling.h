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

/* Block until the SDP offer arrives from master (or timeout / error).
 * The pointer returned via *out_offer is owned by the signaling layer
 * and stays valid until the next signaling call on the same handle. */
int signaling_wait_for_offer(SignalingHandle sig, const char **out_offer);

/* Send our SDP answer back to master. */
int signaling_send_answer(SignalingHandle sig, const char *sdp_answer);

#endif /* WEBRTC_SIGNALING_H_ */
