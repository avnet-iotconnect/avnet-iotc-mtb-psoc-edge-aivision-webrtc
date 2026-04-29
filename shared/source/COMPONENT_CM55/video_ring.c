/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * CM55-side producer for the cross-core H.264 NAL ring.  See
 * shared/include/video_ring.h and PILOT.md §5 for the full protocol.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cybsp.h"
#include "core_cm55.h"

#include "video_ring.h"

/* The ring lives at the base of m33_m55_shared (256 KB).  Same fixed
 * pointer trick shared_mem.c uses; no linker fragment needed.
 *   sizeof(header)   = 32 B
 *   2 * sizeof(slot) = 2 * (20 B header + 32 KB payload) ~= 64 KB
 *   total            ~ 64 KB, well within 256 KB. */
#define VIDEO_RING_BASE_ADDR (CYMEM_CM55_0_m33_m55_shared_START)

static volatile video_ring_header_t * const ring_header =
    (volatile video_ring_header_t *)VIDEO_RING_BASE_ADDR;

static video_ring_slot_t * const ring_slots =
    (video_ring_slot_t *)(VIDEO_RING_BASE_ADDR + sizeof(video_ring_header_t));

/* Producer-private state.  Lives in CM55 SRAM, not the shared region. */
static uint32_t producer_idx = 0;
static uint32_t prev_enabled = 0;
static video_ring_producer_stats_t producer_stats = {0};

static inline void clean_range(const void *addr, size_t bytes) {
    SCB_CleanDCache_by_Addr((void *)addr, (int32_t)bytes);
}

void video_ring_init(void) {
    ring_header->magic = 0;
    ring_header->enabled = 0;
    memset((void *)ring_header->reserved, 0, sizeof(ring_header->reserved));

    /* Zero the per-slot flags so a fresh session starts cleanly even if
     * CM33 hasn't run its session_start yet. */
    for (uint32_t i = 0; i < VIDEO_RING_SLOTS; i++) {
        ring_slots[i].available = 0;
        ring_slots[i].length = 0;
        ring_slots[i].pts_ms = 0;
        ring_slots[i].is_idr = 0;
        ring_slots[i].seq = 0;
    }

    producer_idx = 0;
    prev_enabled = 0;
    memset(&producer_stats, 0, sizeof(producer_stats));

    __DMB();
    ring_header->magic = VIDEO_RING_MAGIC;

    clean_range((const void *)ring_header, sizeof(*ring_header));
    clean_range(ring_slots, VIDEO_RING_SLOTS * sizeof(video_ring_slot_t));
}

bool video_ring_try_publish(
    const uint8_t *coded_data,
    uint32_t coded_size,
    uint32_t pts_ms,
    bool is_idr,
    uint32_t seq
) {
    /* 1. Read enabled.  CM33 may have toggled it since last call. */
    uint32_t enabled = ring_header->enabled;

    /* 2. On any 0->1 transition (including first ever), re-arm to slot 0. */
    if (enabled != 0 && prev_enabled == 0) {
        producer_idx = 0;
    }
    prev_enabled = enabled;

    if (enabled == 0) {
        producer_stats.dropped_disabled++;
        return false;
    }

    if (coded_size > VIDEO_RING_SLOT_BYTES) {
        producer_stats.dropped_oversize++;
        return false;
    }

    /* 3. Check the target slot.  CM33 hasn't released it yet -> drop,
     *    do NOT advance producer_idx.  Slot ownership and ordering are
     *    preserved across the drop. */
    video_ring_slot_t *slot = &ring_slots[producer_idx];
    if (slot->available != 0U) {
        producer_stats.dropped_busy++;
        return false;
    }

    /* 4. Write payload + metadata.  Only the producer touches these
     *    fields while available==0, so no race. */
    memcpy(slot->payload, coded_data, coded_size);
    slot->length = coded_size;
    slot->pts_ms = pts_ms;
    slot->is_idr = is_idr ? 1U : 0U;
    slot->seq = seq;

    /* 5. Order the metadata writes ahead of the available flip, then
     *    flush the slot to memory so CM33 can see it. */
    clean_range(slot, offsetof(video_ring_slot_t, payload) + coded_size);
    __DMB();
    slot->available = 1U;
    clean_range(slot, sizeof(slot->available));

    producer_stats.published++;
    producer_idx = 1U - producer_idx;
    return true;
}

const video_ring_header_t *video_ring_header(void) {
    return (const video_ring_header_t *)ring_header;
}

const video_ring_producer_stats_t *video_ring_producer_stats(void) {
    return &producer_stats;
}
