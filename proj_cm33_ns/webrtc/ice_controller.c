/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

/* M3 stub. Real impl pending: awslabs ICE wrapper, host candidate gather
 * from lwIP netif list, STUN binding requests against the region-templated
 * URL, pair table (~32 entries), connectivity-check loop driven from
 * peer_connection_run(). Shares the UDP socket with dtls_transport. */

#include <stdio.h>

#include "webrtc/ice_controller.h"

int ice_controller_start(PeerConnectionHandle pc, const AwsCreds *creds) {
    (void) pc;
    printf("ice_controller_start: STUB (region=%s)\n",
        creds && creds->region ? creds->region : "<null>");
    return 0;
}

void ice_controller_stop(PeerConnectionHandle pc) {
    (void) pc;
    printf("ice_controller_stop: STUB\n");
}
