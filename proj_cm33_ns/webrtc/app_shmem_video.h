/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * CM33-NS dummy consumer of the H.264 NAL ring produced by CM55.
 *
 * Lifecycle:
 *   - app_shmem_video_init()   -- create the (idle) FreeRTOS task once,
 *                                 from main() before the scheduler starts
 *                                 (or any time after).
 *   - app_shmem_video_start()  -- begin a consumption session: opens the
 *                                 ring (clears slot flags, sets enabled=1)
 *                                 and unblocks the worker.
 *   - app_shmem_video_stop()   -- end the session: clears enabled=0 and
 *                                 parks the worker until the next start.
 *
 * The worker for now is a stub: it polls the ring, prints one line per
 * received NAL with seq/length/is_idr, plus a summary every N frames.
 */

#ifndef APP_SHMEM_VIDEO_H_
#define APP_SHMEM_VIDEO_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

typedef struct PeerConnectionSession PeerConnectionSession_t;
typedef struct Transceiver Transceiver_t;

void app_shmem_video_init(void);
bool app_shmem_video_start(PeerConnectionSession_t *session, Transceiver_t *transceiver);
void app_shmem_video_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SHMEM_VIDEO_H_ */
