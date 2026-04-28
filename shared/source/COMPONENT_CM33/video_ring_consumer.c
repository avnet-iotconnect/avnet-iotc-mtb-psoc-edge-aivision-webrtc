/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * CM33-side consumer for the cross-core H.264 NAL ring.  See
 * shared/include/video_ring.h and PILOT.md §5 for the full protocol.
 *
 * CM33 on this part has no D-cache, so we only need __DMB() to order
 * shared-memory accesses against CM55's writes; no cache maintenance.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "cybsp.h"
#include "core_cm33.h"

#include "video_ring.h"

#define VIDEO_RING_BASE_ADDR    (CYMEM_CM33_0_m33_m55_shared_START)

static volatile video_ring_header_t * const ring_header = (volatile video_ring_header_t *)VIDEO_RING_BASE_ADDR;
static volatile video_ring_slot_t * const ring_slots = (volatile video_ring_slot_t *)(VIDEO_RING_BASE_ADDR + sizeof(video_ring_header_t));

/* Consumer-private state.  Lives in CM33 SRAM, not the shared region. */
static uint32_t consumer_idx = 0;
static bool     peek_outstanding = false;

void video_ring_consumer_session_start(void) {
    /* Wipe any leftover frames from a prior session before re-enabling.
     * CM55 is guaranteed to see available=0 before enabled=1 thanks to
     * the __DMB() and the fact that CM55 only consults available after
     * it observes enabled=1. */
    for (uint32_t i = 0; i < VIDEO_RING_SLOTS; i++) {
        ring_slots[i].available = 0U;
    }
    consumer_idx     = 0;
    peek_outstanding = false;

    __DMB();
    ring_header->enabled = 1U;
    __DMB();
}

void video_ring_consumer_session_stop(void) {
    ring_header->enabled = 0U;
    __DMB();
}

bool video_ring_consumer_try_peek(video_ring_slot_view_t *out) {
    if (out == NULL) {
        return false;
    }
    if (peek_outstanding) {
        /* Caller forgot to release the previous peek.  Safer to refuse
         * than hand them the same slot twice. */
        return false;
    }

    volatile video_ring_slot_t *slot = &ring_slots[consumer_idx];
    if (slot->available == 0U) {
        return false;
    }
    __DMB();

    out->payload = (const uint8_t *)slot->payload;
    out->length = slot->length;
    out->pts_ms = slot->pts_ms;
    out->is_idr = (slot->is_idr != 0U);
    out->seq = slot->seq;

    peek_outstanding = true;
    return true;
}

void video_ring_consumer_release(void) {
    if (!peek_outstanding) {
        return;
    }

    volatile video_ring_slot_t *slot = &ring_slots[consumer_idx];
    __DMB();
    slot->available = 0U;
    __DMB();

    consumer_idx     = 1U - consumer_idx;
    peek_outstanding = false;
}
