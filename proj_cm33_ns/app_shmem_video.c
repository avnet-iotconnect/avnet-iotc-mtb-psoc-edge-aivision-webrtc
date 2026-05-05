/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * CM33-NS dummy consumer of the H.264 NAL ring produced by CM55.
 * See app_shmem_video.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cybsp.h"
#include "FreeRTOS.h"
#include "task.h"

#include "video_ring.h"
#include "app_shmem_video.h"

#include "webrtc_smoke_test.h" // TEMP SMOKE TEST. TODO: remove this and the test files after testing.


#define APP_SHMEM_VIDEO_TASK_NAME       ("CM33 Shmem Video")
#define APP_SHMEM_VIDEO_TASK_STACK      (10U * 1024U)
// Below app_task (priority 2) so TLS handshakes during SDK init aren't
// starved by the busy-poll consumer once CM55 starts pumping frames.
#define APP_SHMEM_VIDEO_TASK_PRIORITY   (tskIDLE_PRIORITY + 1U)

/* Polling cadence.  PILOT.md §5.4 calls for 20 ms; encoder runs at
 * ~250 ms per frame so we have ~12x headroom on the consumer side. */
#define APP_SHMEM_VIDEO_POLL_MS         (20U)

/* How often to emit a summary line, in received frames. */
#define APP_SHMEM_VIDEO_REPORT_EVERY    (30U)


static TaskHandle_t shmem_video_task_handle = NULL;
static volatile bool shmem_video_running = false;


static void shmem_video_task(void *arg) {
    (void)arg;

    uint32_t frames_total = 0;
    uint32_t bytes_total = 0;
    uint32_t idr_total = 0;
    uint32_t window_frames = 0;
    uint32_t window_bytes = 0;

    webrtc_smoke_test_run(); // TEMP SMOKE TEST. TODO: remove this and the test files after testing.

    for (;;) {
        if (!shmem_video_running) {
            vTaskDelay(pdMS_TO_TICKS(APP_SHMEM_VIDEO_POLL_MS));
            continue;
        }

        video_ring_slot_view_t view;
        if (!video_ring_consumer_try_peek(&view)) {
            vTaskDelay(pdMS_TO_TICKS(APP_SHMEM_VIDEO_POLL_MS));
            continue;
        }

        /* Stub consumer: just inspect and report.  Real WebRTC media
         * source will copy / hand off here before releasing. */
        frames_total++;
        bytes_total += view.length;
        window_frames++;
        window_bytes += view.length;
        if (view.is_idr) {
            idr_total++;
        }

        // printf("[shmemv] seq=%u len=%u is_idr=%d pts_ms=%u\n", (unsigned)view.seq, (unsigned)view.length, (int)view.is_idr, (unsigned)view.pts_ms);

        video_ring_consumer_release();

        if (window_frames >= APP_SHMEM_VIDEO_REPORT_EVERY) {
#if 0
            printf("[shmemv] summary: total=%u bytes=%u idr=%u (window: %u frames, %u bytes)\n",
                (unsigned)frames_total, (unsigned)bytes_total, (unsigned)idr_total, (unsigned)window_frames, (unsigned)window_bytes
            );
#endif            
            window_frames = 0;
            window_bytes = 0;
        }

    }
}


void app_shmem_video_init(void) {
    if (shmem_video_task_handle != NULL) {
        return;
    }
    BaseType_t ok = xTaskCreate(shmem_video_task, APP_SHMEM_VIDEO_TASK_NAME, APP_SHMEM_VIDEO_TASK_STACK,
        NULL, APP_SHMEM_VIDEO_TASK_PRIORITY, &shmem_video_task_handle
    );
    if (pdPASS != ok) {
        printf("[shmemv] failed to create task\n");
        shmem_video_task_handle = NULL;
    }
}


bool app_shmem_video_start(void) {
    if (shmem_video_task_handle == NULL) {
        printf("[shmemv] start before init\n");
        return false;
    }
    if (shmem_video_running) {
        return true;
    }

    video_ring_consumer_session_start();
    shmem_video_running = true;
    printf("[shmemv] session started\n");
    return true;
}


void app_shmem_video_stop(void) {
    if (!shmem_video_running) {
        return;
    }
    shmem_video_running = false;
    video_ring_consumer_session_stop();
    printf("[shmemv] session stopped\n");
}
