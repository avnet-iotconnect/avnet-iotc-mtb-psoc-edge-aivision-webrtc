/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * S6a stub. The companion `app_webrtc.c.bak` is the prior hand-rolled
 * implementation kept under version control for reference while the
 * orchestrator is rewritten on top of the upstream awslabs PeerConnection /
 * IceController / SignalingController APIs.
 *
 * Scope of this stub:
 *   - Preserve the public surface declared in app_webrtc.h so external
 *     callers (main.c, app_task.c) keep linking.
 *   - Boot a FreeRTOS task that idles. No WebRTC work happens.
 *   - Land in S6a as part of making the build pass after the upstream
 *     algorithm code lands; the body fills in during S6b when the offer
 *     parser is wired in.
 *
 * Notes for the next writer:
 *   - The .bak shows the prior platform plumbing (creds populate, region
 *     parse from ARN, signaling/wslay/ICE wiring, retry/backoff). Useful as
 *     a guide for what hooks still need to land. Do NOT call it through
 *     `#include` — it isn't part of the build.
 *   - Device role is MASTER (we serve video). The .bak's `viewer` wording
 *     and `ConnectAsViewer` API call are wrong per GUIDELINES.md; the new
 *     body uses ConnectAsMaster.
 */

#include <stdbool.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "webrtc/app_webrtc.h"

#define WEBRTC_TASK_NAME       "webrtc"
#define WEBRTC_TASK_STACK_W    (1024U)  // words; revisit when real body lands
#define WEBRTC_TASK_PRIO       (tskIDLE_PRIORITY + 2)

static TaskHandle_t s_webrtc_task = NULL;
static bool s_start_requested = false;
static bool s_creds_dirty = false;


static void webrtc_task_stub(void *arg) {
    (void) arg;
    printf("[webrtc] stub task running (S6a — no work, waiting on S6b body)\n");
    for (;;) {
        // Idle. The real body lands in S6b.
        // s_start_requested / s_creds_dirty are observable here when wired.
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}


void app_webrtc_init(void) {
    if (NULL != s_webrtc_task) {
        return;
    }
    BaseType_t rc = xTaskCreate(
        webrtc_task_stub,
        WEBRTC_TASK_NAME,
        WEBRTC_TASK_STACK_W,
        NULL,
        WEBRTC_TASK_PRIO,
        &s_webrtc_task
    );
    if (pdPASS != rc) {
        printf("[webrtc] xTaskCreate failed: %d\n", (int) rc);
        s_webrtc_task = NULL;
    }
}


bool app_webrtc_start(void) {
    s_start_requested = true;
    return NULL != s_webrtc_task;
}


void app_webrtc_stop(void) {
    s_start_requested = false;
    // Real teardown (signaling disconnect, peer connection close, ring
    // consumer release) lands in S6b alongside the orchestration body.
}


void app_webrtc_notify_creds_updated(void) {
    s_creds_dirty = true;
}
