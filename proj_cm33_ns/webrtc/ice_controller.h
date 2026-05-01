/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef WEBRTC_ICE_CONTROLLER_H_
#define WEBRTC_ICE_CONTROLLER_H_

#include "webrtc/aws_creds.h"
#include "webrtc/peer_connection.h"

/* Open the UDP socket(s), gather host candidates, kick STUN against
 * stun.kinesisvideo.<region>.amazonaws.com:443. Pair table sized to ~32
 * (LAN-only no-TURN; see PILOT.md M3 RAM budget).
 *
 * Returns 0 on success once the local candidates are known. The actual
 * pairing/connectivity-check loop runs inside peer_connection_run(). */
int ice_controller_start(PeerConnectionHandle pc, const AwsCreds *creds);
void ice_controller_stop(PeerConnectionHandle pc);

#endif /* WEBRTC_ICE_CONTROLLER_H_ */
