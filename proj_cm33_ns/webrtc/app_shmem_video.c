/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * CM33-NS consumer of the H.264 NAL ring produced by CM55.
 * See app_shmem_video.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cybsp.h"
#include "FreeRTOS.h"
#include "task.h"

#include "video_ring.h"
#include "peer_connection.h"
#include "peer_connection_data_types.h"
#include "app_shmem_video.h"


#define APP_SHMEM_VIDEO_TASK_NAME       ("CM33 Shmem Video")
#define APP_SHMEM_VIDEO_TASK_STACK      (4U * 1024U)
#define APP_SHMEM_VIDEO_TASK_PRIORITY   (tskIDLE_PRIORITY + 1U)

/* Encoder runs at ~250 ms per frame so we have ~12x headroom on the consumer side. */
#define APP_SHMEM_VIDEO_POLL_MS         (20U)

/* How often to emit a summary line, in delivered frames. */
#define APP_SHMEM_VIDEO_REPORT_EVERY    (30U)

extern void memory_test(void);

static TaskHandle_t shmem_video_task_handle = NULL;
static volatile bool shmem_video_running = false;
static PeerConnectionSession_t *shmem_video_session = NULL;
static Transceiver_t *shmem_video_transceiver = NULL;


static void shmem_video_task(void *arg) {
    (void)arg;

    uint32_t frames_total = 0;
    uint32_t bytes_total = 0;
    uint32_t idr_total = 0;
    uint32_t window_frames = 0;
    uint32_t window_bytes = 0;
    uint32_t write_fail_total = 0;

    for (;;) {
        if (!shmem_video_running) {
            vTaskDelay(pdMS_TO_TICKS(APP_SHMEM_VIDEO_POLL_MS));
            continue;
        }

        /* Mirror Ameba's OnMediaSinkHook: only push frames when the PC is
         * fully connected. During setup (state < CONNECTION_READY) or after
         * DTLS-close (state drops back to INITED/CLOSING), the WriteFrame call
         * is a no-op that spams the log with "session is not ready"; the
         * orchestrator will call app_shmem_video_stop() shortly. Until it
         * does, pulse-yield without consuming the ring slot so the encoder
         * doesn't get evicted. */
        if (PEER_CONNECTION_SESSION_STATE_CONNECTION_READY != shmem_video_session->state) {
            vTaskDelay(pdMS_TO_TICKS(APP_SHMEM_VIDEO_POLL_MS));
            continue;
        }

        video_ring_slot_view_t view;
        if (!video_ring_consumer_try_peek(&view)) {
            vTaskDelay(pdMS_TO_TICKS(APP_SHMEM_VIDEO_POLL_MS));
            continue;
        }

        PeerConnectionFrame_t frame = {
            .version = 0,
            .pData = (uint8_t *)view.payload,
            .dataLength = view.length,
            .presentationUs = (uint64_t)view.pts_ms * 1000U,
        };

        PeerConnectionResult_t pcr = PeerConnection_WriteFrame(
            shmem_video_session, shmem_video_transceiver, &frame);
        if (pcr != PEER_CONNECTION_RESULT_OK) {
            write_fail_total++;
        }

        video_ring_consumer_release();

        frames_total++;
        bytes_total += view.length;
        window_frames++;
        window_bytes += view.length;
        if (view.is_idr) {
            idr_total++;
        }

        if (window_frames >= APP_SHMEM_VIDEO_REPORT_EVERY) {
            printf("[shmemv] sent=%u bytes=%u idr=%u fail=%u (window: %u frames, %u bytes)\n",
                (unsigned)frames_total, (unsigned)bytes_total, (unsigned)idr_total,
                (unsigned)write_fail_total, (unsigned)window_frames, (unsigned)window_bytes
            );
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


bool app_shmem_video_start(PeerConnectionSession_t *session, Transceiver_t *transceiver) {
    if (shmem_video_task_handle == NULL) {
        printf("[shmemv] start before init\n");
        return false;
    }
    if (session == NULL || transceiver == NULL) {
        printf("[shmemv] start with NULL session/transceiver\n");
        return false;
    }
    if (shmem_video_running) {
        return true;
    }

    shmem_video_session = session;
    shmem_video_transceiver = transceiver;
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
    shmem_video_session = NULL;
    shmem_video_transceiver = NULL;
    printf("[shmemv] session stopped\n");
}
