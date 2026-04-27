/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#include <string.h>

#include "cybsp.h"
#include "core_cm55.h"

#include "video_ring.h"

/* Place the ring at the base of m33_m55_shared, accessed via fixed
 * pointer.  Same approach shared_mem.c uses for oob_shared_data_t -- no
 * linker fragment needed.  Total footprint:
 *   sizeof(header)   = 32 B
 *   6 * sizeof(slot) = 6 * (16 B header + 32 KB payload) = ~196 KB
 *   total            < 200 KB, fits in 256 KB region. */
#define VIDEO_RING_BASE_ADDR    (CYMEM_CM55_0_m33_m55_shared_START)

static volatile video_ring_header_t * const ring_header =
    (volatile video_ring_header_t *)VIDEO_RING_BASE_ADDR;

static video_ring_slot_t * const ring_slots =
    (video_ring_slot_t *)(VIDEO_RING_BASE_ADDR + sizeof(video_ring_header_t));

static inline void clean_range(const void *addr, size_t bytes)
{
    SCB_CleanDCache_by_Addr((void *)addr, (int32_t)bytes);
}

void video_ring_init(void)
{
    ring_header->magic       = 0;
    ring_header->slot_count  = VIDEO_RING_SLOTS;
    ring_header->slot_bytes  = VIDEO_RING_SLOT_BYTES;
    ring_header->write_index = 0;
    ring_header->read_index  = 0;
    ring_header->dropped     = 0;
    ring_header->reserved[0] = 0;
    ring_header->reserved[1] = 0;
    __DMB();
    ring_header->magic       = VIDEO_RING_MAGIC;
    clean_range((const void *)ring_header, sizeof(*ring_header));
}

video_ring_slot_t *video_ring_acquire_write_slot(void)
{
    uint32_t w = ring_header->write_index;
    uint32_t r = ring_header->read_index;
    /* Drop-tail policy: if the producer would lap the consumer, the
     * oldest unread slot is the one we are about to overwrite.  Bump
     * read_index forward so the consumer sees the loss instead of
     * reading a half-written slot. */
    if ((w - r) >= VIDEO_RING_SLOTS) {
        ring_header->read_index = w - (VIDEO_RING_SLOTS - 1);
        ring_header->dropped++;
    }
    return &ring_slots[w % VIDEO_RING_SLOTS];
}

void video_ring_publish(video_ring_slot_t *slot,
                        uint32_t length,
                        uint32_t pts_ms,
                        bool     is_idr)
{
    if (length > VIDEO_RING_SLOT_BYTES) {
        length = VIDEO_RING_SLOT_BYTES;
    }
    slot->length = length;
    slot->pts_ms = pts_ms;
    slot->is_idr = is_idr ? 1U : 0U;
    slot->seq    = ring_header->write_index;

    /* Push the payload + slot header out of CM55's D-cache before the
     * consumer can see the new write_index. */
    clean_range(slot, sizeof(*slot) - VIDEO_RING_SLOT_BYTES + length);
    __DMB();

    ring_header->write_index = ring_header->write_index + 1U;
    clean_range((const void *)ring_header, sizeof(*ring_header));
}

const video_ring_header_t *video_ring_header(void)
{
    return (const video_ring_header_t *)ring_header;
}
