/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef PROJ_CM55_ENCODER_TASK_H_
#define PROJ_CM55_ENCODER_TASK_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "inference_task.h"

#define ENCODER_TASK_NAME           ("CM55 Encoder Task")
#define ENCODER_TASK_STACK_SIZE     (8U * 1024U)
#define ENCODER_TASK_PRIORITY       (configMAX_PRIORITIES - 3)

/* Encoded frame is 320x240 BGR565, matching the camera.  Bounding boxes,
 * when enabled, are drawn in camera-space coordinates with identity scale
 * so they land exactly where they would on the visible portion of the
 * LCD composite. */
#define ENCODER_FRAME_WIDTH         (320)
#define ENCODER_FRAME_HEIGHT        (240)

/* Toggle overlays on the encoded frame at runtime. Default on. */
extern bool encoder_draw_overlays;

/* Init split in two: semaphore init is safe before the scheduler starts
 * (call from main before vTaskStartScheduler).  VGLite-backed buffer
 * allocation and the actual task launch must happen after vg_lite_init()
 * has run inside cm55_ns_gfx_task -- see encoder_task_start_after_vglite(). */
void encoder_task_early_init(void);
void encoder_task_start_after_vglite(void);

void cm55_encoder_task(void *arg);

/* Called from cm55_ns_gfx_task right after VG_switch_frame().  The display
 * has already presented the frame and inference has already consumed
 * dvp_bgr565_frames[active_frame], so the source buffer is stable long
 * enough for us to copy it.  Safe no-op before
 * encoder_task_start_after_vglite() has run. */
void encoder_on_display_frame_done(prediction_od_t *pred);

#ifdef __cplusplus
}
#endif

#endif /* PROJ_CM55_ENCODER_TASK_H_ */
