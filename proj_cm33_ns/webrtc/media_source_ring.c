/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

/* M3 stub. Real impl pending: small task that does video_ring_consumer_*,
 * forwards each slot view to PeerConnection_WriteFrame() with no copy, then
 * releases the slot. Replaces the print-stub body in app_shmem_video.c
 * (Step 4). */

#include <stdio.h>

#include "webrtc/media_source_ring.h"

int media_source_ring_start(PeerConnectionHandle pc) {
    (void) pc;
    printf("media_source_ring_start: STUB\n");
    return 0;
}

void media_source_ring_stop(void) {
    printf("media_source_ring_stop: STUB\n");
}
