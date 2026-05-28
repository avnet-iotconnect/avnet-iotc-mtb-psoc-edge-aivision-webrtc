/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * Cross-core ring buffer for H.264 NAL units.
 *
 * Layout (in m33_m55_shared, 256 KB region at 0x26480000):
 *
 *   +---------------------------+
 *   | video_ring_header_t       |  small, fixed offset 0
 *   +---------------------------+
 *   | slot[0]                   |  payload + per-slot metadata
 *   | slot[1]                   |
 *   +---------------------------+
 *
 * Two-slot drop-tail ring with explicit per-slot ownership flags and a
 * session-level enable flag.  See PILOT.md §5.1 for the full protocol.
 *
 * Ownership rules (single-writer per state transition):
 *   - enabled       -- written only by CM33; read by both.
 *   - available[i]  -- 0->1 by CM55 (producer); 1->0 by CM33 (consumer);
 *                      read by both.
 *   - payload + length + pts_ms + is_idr + seq -- written by CM55 before
 *                      it sets available=1; read by CM33 only when
 *                      available==1.
 *
 * No atomics needed; __DMB() ordering plus cache maintenance on CM55
 * (which is the only core with a D-cache on this part) is sufficient.
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
#define VIDEO_RING_SLOTS        (2)
#define VIDEO_RING_SLOT_BYTES   (32 * 1024)

typedef struct {
    uint32_t available; // 0 = empty/consumed, 1 = filled by CM55
    uint32_t length; // bytes written into payload
    uint32_t pts_ms; // producer-side timestamp
    uint32_t is_idr; // nonzero if frame contains an IDR
    uint32_t seq; // informational frame counter
    uint8_t payload[VIDEO_RING_SLOT_BYTES];
} video_ring_slot_t;

typedef struct {
    uint32_t magic; // VIDEO_RING_MAGIC once initialized
    uint32_t enabled; // 0 = paused, 1 = streaming -- written by CM33
    uint32_t reserved[6];
} video_ring_header_t;

/* ---- Producer side (CM55) ---- */

/* One-time init.  Called from CM55 startup.  Zeroes the header, leaves
 * enabled=0 so CM33 must explicitly start a session before any encode
 * work occurs. */
void video_ring_init(void);

/* Per-frame producer protocol -- see PILOT.md §5.3.  Returns true if the
 * frame was published, false if it was dropped (either because the
 * session is disabled or the next slot is still occupied).  When this
 * returns false the caller's encoded bytes are discarded. */
bool video_ring_try_publish(
    const uint8_t *coded_data,
    uint32_t coded_size,
    uint32_t pts_ms,
    bool is_idr,
    uint32_t seq
);

/* Diagnostics -- read-only view of header for logging. */
const video_ring_header_t *video_ring_header(void);

/* Producer-side counters (CM55-private; informational only). */
typedef struct {
    uint32_t published; // slots successfully filled
    uint32_t dropped_busy; // next slot still held by CM33
    uint32_t dropped_disabled; // enabled==0 at start of frame
    uint32_t dropped_oversize; // coded_size > VIDEO_RING_SLOT_BYTES
} video_ring_producer_stats_t;

const video_ring_producer_stats_t *video_ring_producer_stats(void);

/* ---- Consumer side (CM33) ---- */

typedef struct {
    const uint8_t *payload;
    uint32_t length;
    uint32_t pts_ms;
    bool is_idr;
    uint32_t seq;
} video_ring_slot_view_t;

/* Session start: clears both slot available flags, then sets
 * enabled=1.  Caller (CM33) must invoke this before CM55 begins
 * encoding for a session.  Resets internal consumer index. */
void video_ring_consumer_session_start(void);

/* Session stop: sets enabled=0.  Any in-flight encode on CM55 may still
 * land a NAL in a slot; the next session_start wipes it. */
void video_ring_consumer_session_stop(void);

/* Returns true and fills *out if the consumer's current slot is full.
 * The view's payload pointer is valid only until the next call to
 * video_ring_consumer_release().  Returns false (no-op) when the slot is
 * empty. */
bool video_ring_consumer_try_peek(video_ring_slot_view_t *out);

/* Marks the slot last returned by try_peek as consumed (available=0)
 * and advances the consumer index.  Must be paired with try_peek. */
void video_ring_consumer_release(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_RING_H_ */
