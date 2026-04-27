/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * Cross-core ring buffer for H.264 NAL units.
 *
 * Layout (in m33_m55_shared, 256 KB region at 0x26480000):
 *
 *   +---------------------------+
 *   | video_ring_header_t       |  small, fixed offset
 *   +---------------------------+
 *   | slot[0]: 32 KB            |  payload slots
 *   | slot[1]: 32 KB            |
 *   |   ...                     |
 *   | slot[VIDEO_RING_SLOTS-1]  |
 *   +---------------------------+
 *
 * Producer (CM55 encoder):
 *   1. Acquire write slot index = header.write_index
 *   2. Fill slot bytes (NAL stream)
 *   3. Set slot.length, slot.pts_ms, slot.is_idr, slot.seq
 *   4. SCB_CleanDCache_by_Addr(slot)
 *   5. header.write_index++ (with cache clean)
 *   6. (Future) fire IPC pipe notification to CM33
 *
 * Consumer (CM33, not built yet):
 *   1. While header.read_index != header.write_index:
 *        SCB_InvalidateDCache_by_Addr(slot)
 *        process slot
 *        header.read_index++
 *
 * Single-producer / single-consumer; integer wrap on indices, modulo
 * VIDEO_RING_SLOTS for slot lookup.  If the producer laps the consumer
 * (write - read >= VIDEO_RING_SLOTS) the oldest slot is overwritten.
 * Acceptable for live-video drop-tail behavior; not a reliable transport.
 */

#ifndef VIDEO_RING_H_
#define VIDEO_RING_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VIDEO_RING_MAGIC        (0x56524E47U)  /* 'VRNG' */
#define VIDEO_RING_SLOTS        (6)
#define VIDEO_RING_SLOT_BYTES   (32 * 1024)

typedef struct {
    uint32_t length;     /* bytes written into payload */
    uint32_t pts_ms;     /* producer-side timestamp */
    uint32_t is_idr;     /* nonzero if frame contains an IDR */
    uint32_t seq;        /* monotonic frame counter */
    uint8_t  payload[VIDEO_RING_SLOT_BYTES];
} video_ring_slot_t;

typedef struct {
    uint32_t magic;          /* VIDEO_RING_MAGIC once initialized */
    uint32_t slot_count;     /* VIDEO_RING_SLOTS */
    uint32_t slot_bytes;     /* VIDEO_RING_SLOT_BYTES */
    uint32_t write_index;    /* producer-owned, advances monotonically */
    uint32_t read_index;     /* consumer-owned, advances monotonically */
    uint32_t dropped;        /* producer-counted laps over consumer */
    uint32_t reserved[2];
} video_ring_header_t;

/* Producer side (CM55) */
void video_ring_init(void);
video_ring_slot_t *video_ring_acquire_write_slot(void);
void video_ring_publish(video_ring_slot_t *slot,
                        uint32_t length,
                        uint32_t pts_ms,
                        bool     is_idr);

/* Diagnostics */
const video_ring_header_t *video_ring_header(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_RING_H_ */
