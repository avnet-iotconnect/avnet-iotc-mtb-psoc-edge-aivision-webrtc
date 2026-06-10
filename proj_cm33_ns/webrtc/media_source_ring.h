/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef WEBRTC_MEDIA_SOURCE_RING_H_
#define WEBRTC_MEDIA_SOURCE_RING_H_

#include "peer_connection.h"

/* Drains the CM55->CM33 NAL ring (shared/include/video_ring.h) and feeds
 * each slot directly to PeerConnection_WriteFrame() - no copy. The ring
 * slot stays reserved until WriteFrame returns; CM55 encodes the next
 * frame into the other slot in parallel.
 *
 * Started by peer_connection.c once DTLS is up and SRTP is keyed. */
int media_source_ring_start(PeerConnectionHandle pc);
void media_source_ring_stop(void);

#endif /* WEBRTC_MEDIA_SOURCE_RING_H_ */
