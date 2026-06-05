/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "cybsp.h"
#include "cyabs_rtos.h"
#include "cyabs_rtos_impl.h"
#include "FreeRTOS.h"
#include "task.h"

#include "vg_lite.h"
#include "ifx_time_utils.h"

#include "inference_task.h"
#include "lcd_task.h"
#include "encoder_task.h"
#include "video_ring.h"

/* minih264 is a single-header library; this TU owns the implementation. */
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"

/* Runtime toggle for bbox overlays on the encoded frame.  Default on;
 * flip at runtime (e.g. from a debugger) to compare clean vs. annotated
 * output without a rebuild. */
bool encoder_draw_overlays = true;

/* Stable per-frame camera snapshot owned by lcd_task.  Filled in
 * draw() Stage 1 (identity blit from dvp_bgr565_frames[active_frame]).
 *
 * We source the encoder blit from this rather than dvp_bgr565_frames
 * directly for two reasons:
 *   1. Consistency -- inference already reads bgr565 (lcd_task.c's
 *      ifx_image_conv_RGB565_to_RGB888_i8 call), so encoder + display +
 *      inference all consume the same per-frame snapshot.
 *   2. Avoids a second AXI read of the buffer the AXIDMAC is concurrently
 *      writing -- subjective reduction in the DVP line-corruption
 *      artifacts observed when WebRTC is streaming.
 *
 * Revert option: change &bgr565 below to &dvp_bgr565_frames[active_frame]
 * (and re-add the externs) if the line-corruption root cause is fixed
 * upstream and you'd rather pull straight from the live DMA buffer.
 * See work/reference/LINE_CORRUPTION_ISSUE.md. */
extern vg_lite_buffer_t bgr565;

/* Dedicated encoder frame.  Separate from bgr565 so we can scribble
 * overlays on it without corrupting what inference re-reads every
 * frame, and so its lifetime is owned by the encoder task. */
static vg_lite_buffer_t encoder_frame;

/* Center-160 H crop stretched 2x H into encoder_frame.  Built once at
 * task start (encoder_task_start_after_vglite); used per-frame in the
 * source->encoder_frame blit.  See work/reference/DVP_ISSUE.md Issue 1. */
static vg_lite_matrix_t encoder_crop_matrix;

/* Snapshot of the predictions that match whatever camera frame we just
 * copied.  Taken under the gfx task so it's self-consistent with the
 * frame being handed off. */
static prediction_od_t encoder_prediction;

static cy_semaphore_t encoder_semaphore;
static cy_thread_t    encoder_thread;
static volatile bool  encoder_ready = false;

/* Producer-side cadence: incremented once per gfx-task hook.  The encoder
 * task reads-and-clears at the same window it reports tot_ms, so the ratio
 * to encoded-frames gives both the input fps and the input-drop fraction. */
static volatile uint32_t encoder_hook_fires = 0;

/* Pipeline-stage counters bumped by inference and gfx tasks. */
static volatile uint32_t encoder_inf_done   = 0;
static volatile uint32_t encoder_disp_done  = 0;

void encoder_count_inference_done(void) { encoder_inf_done++; }
void encoder_count_display_present(void) { encoder_disp_done++; }

/* minih264 working set, sized from M0 measurements at 320x240 plain-C.
 * H264E_sizeof() is checked at init and we abort if reality exceeds the
 * pool. */
#define ENCODER_PERSIST_BYTES   (300 * 1024)
#define ENCODER_SCRATCH_BYTES   (200 * 1024)
#define ENCODER_I420_BYTES      ((ENCODER_FRAME_WIDTH) * (ENCODER_FRAME_HEIGHT) * 3 / 2)

#define ENCODER_TARGET_FPS      (5)
#define ENCODER_GOP             (60)
#define ENCODER_BITRATE_BPS     (400 * 1000)
/* Rate-limit how often the gfx hook hands a frame to the encoder.  This is
 * intentionally lower than the camera/display cadence so skipped frames avoid
 * the encoder hook blit, the BGR565->I420 conversion, H.264 encode, and ring
 * publish entirely.  Tune for DVP corruption experiments. */
#define ENCODER_MAX_INPUT_FPS   (4U)
#define ENCODER_MIN_INPUT_MS    (1000U / ENCODER_MAX_INPUT_FPS)

__attribute__((section(".cy_socmem_data"), aligned(16)))
static uint8_t encoder_persist[ENCODER_PERSIST_BYTES];

__attribute__((section(".cy_socmem_data"), aligned(16)))
static uint8_t encoder_scratch[ENCODER_SCRATCH_BYTES];

__attribute__((section(".cy_socmem_data"), aligned(16)))
static uint8_t encoder_i420[ENCODER_I420_BYTES];

static H264E_create_param_t encoder_create_param;
static H264E_run_param_t encoder_run_param;
static H264E_io_yuv_t encoder_io;
static uint32_t encoder_frame_seq = 0;
static bool encoder_input_seen = false;
static uint32_t encoder_last_input_ms = 0;

/* Overlay colors, matching lcd_task.c's palette: {black, green, red, blue}. */
#define BGR565_PACK(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

static const uint16_t overlay_color_by_cid[4] = {
    BGR565_PACK(0,   0,   0),    /* 0: black  */
    BGR565_PACK(0,   255, 0),    /* 1: green  */
    BGR565_PACK(227, 66,  24),   /* 2: red    */
    BGR565_PACK(8,   24,  168),  /* 3: blue   */
};

static inline void put_pixel_clipped(uint16_t *buf, int x, int y, uint16_t color) {
    if ((unsigned)x >= ENCODER_FRAME_WIDTH)  return;
    if ((unsigned)y >= ENCODER_FRAME_HEIGHT) return;
    buf[y * ENCODER_FRAME_WIDTH + x] = color;
}

static void draw_hline(uint16_t *buf, int x0, int x1, int y, uint16_t color) {
    if ((unsigned)y >= ENCODER_FRAME_HEIGHT) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= ENCODER_FRAME_WIDTH) x1 = ENCODER_FRAME_WIDTH - 1;
    uint16_t *row = buf + y * ENCODER_FRAME_WIDTH;
    for (int x = x0; x <= x1; x++) row[x] = color;
}

static void draw_vline(uint16_t *buf, int x, int y0, int y1, uint16_t color) {
    if ((unsigned)x >= ENCODER_FRAME_WIDTH) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 >= ENCODER_FRAME_HEIGHT) y1 = ENCODER_FRAME_HEIGHT - 1;
    for (int y = y0; y <= y1; y++) {
        buf[y * ENCODER_FRAME_WIDTH + x] = color;
    }
}

/* BGR565 (little-endian, 5/6/5) -> I420 planar.
 * minih264 wants Y in [16..235] approx + Cb/Cr in [16..240] -- but the
 * encoder is fairly tolerant of full-range input.  We use the standard
 * BT.601 limited-range matrix with rounding and clamp.  Plain C, no
 * SIMD; this is the first thing to Helium-tune if encode_total_ms is
 * dominated by the conversion. */
static inline uint8_t clamp_u8(int v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static void bgr565_to_i420(
    const uint16_t *src, int width, int height,
    uint8_t *y_plane, uint8_t *u_plane, uint8_t *v_plane
) {
    /* Process two rows at a time so we can subsample U/V cleanly. */
    for (int j = 0; j < height; j += 2) {
        const uint16_t *row0 = src + (j + 0) * width;
        const uint16_t *row1 = src + (j + 1) * width;
        uint8_t *y0 = y_plane + (j + 0) * width;
        uint8_t *y1 = y_plane + (j + 1) * width;
        uint8_t *u  = u_plane + (j / 2) * (width / 2);
        uint8_t *v  = v_plane + (j / 2) * (width / 2);

        for (int i = 0; i < width; i += 2) {
            uint16_t p00 = row0[i + 0];
            uint16_t p01 = row0[i + 1];
            uint16_t p10 = row1[i + 0];
            uint16_t p11 = row1[i + 1];

            /* VG_LITE_BGR565 stores channels LSB->MSB as B,G,R, so in the
             * 16-bit word (MSB->LSB) the layout is rrrrr gggggg bbbbb:
             * R is the top 5 bits, B the low 5.  (Matches BGR565_PACK above
             * and the colors the LCD renders.) */
            int r00 = (p00 & 0xF800) >> 8;  /* top 5 bits = R, expand 5 -> 8 */
            int g00 = (p00 & 0x07E0) >> 3;
            int b00 = (p00 & 0x001F) << 3;  /* low 5 bits = B, expand 5 -> 8 */

            int r01 = (p01 & 0xF800) >> 8;
            int g01 = (p01 & 0x07E0) >> 3;
            int b01 = (p01 & 0x001F) << 3;

            int r10 = (p10 & 0xF800) >> 8;
            int g10 = (p10 & 0x07E0) >> 3;
            int b10 = (p10 & 0x001F) << 3;

            int r11 = (p11 & 0xF800) >> 8;
            int g11 = (p11 & 0x07E0) >> 3;
            int b11 = (p11 & 0x001F) << 3;

            /* BT.601 limited-range: Y = 0.257R + 0.504G + 0.098B + 16 */
            y0[i + 0] = clamp_u8((66 * r00 + 129 * g00 +  25 * b00 + 128 + (16 << 8)) >> 8);
            y0[i + 1] = clamp_u8((66 * r01 + 129 * g01 +  25 * b01 + 128 + (16 << 8)) >> 8);
            y1[i + 0] = clamp_u8((66 * r10 + 129 * g10 +  25 * b10 + 128 + (16 << 8)) >> 8);
            y1[i + 1] = clamp_u8((66 * r11 + 129 * g11 +  25 * b11 + 128 + (16 << 8)) >> 8);

            /* Average 2x2 RGB block, then convert to U/V. */
            int r_avg = (r00 + r01 + r10 + r11) >> 2;
            int g_avg = (g00 + g01 + g10 + g11) >> 2;
            int b_avg = (b00 + b01 + b10 + b11) >> 2;

            /* U = -0.148R - 0.291G + 0.439B + 128
             * V =  0.439R - 0.368G - 0.071B + 128 */
            u[i / 2] = clamp_u8(((-38 * r_avg -  74 * g_avg + 112 * b_avg + 128) >> 8) + 128);
            v[i / 2] = clamp_u8((( 112 * r_avg -  94 * g_avg -  18 * b_avg + 128) >> 8) + 128);
        }
    }
}

static void draw_rect_outline(uint16_t *buf, int x0, int y0, int x1, int y1, uint16_t color) {
    draw_hline(buf, x0, x1, y0, color);
    draw_hline(buf, x0, x1, y1, color);
    draw_vline(buf, x0, y0, y1, color);
    draw_vline(buf, x1, y0, y1, color);
    (void)put_pixel_clipped; /* reserved for future corner markers */
}

/* Mirrors the coordinate math in lcd_task.c update_box_data() but with
 * scale=1.0 and offset=0, which is what lcd_task.c resolves to when
 * the target is 320x240 BGR565.  Bboxes outside the visible 320x240
 * strip get clipped, same as on the display. */
static void draw_prediction_overlays(vg_lite_buffer_t *buf, const prediction_od_t *pred) {
    uint16_t *pixels = (uint16_t *)buf->memory;
    for (int32_t i = 0; i < pred->count; i++) {
        int32_t jj  = i << 2;
        int32_t id  = pred->class_id[i];
        int32_t cid = (id >= 0) ? ((id % 3) + 1) : 0;

        int x0 = pred->bbox_int16[jj + 0];
        int y0 = pred->bbox_int16[jj + 1];
        int x1 = pred->bbox_int16[jj + 2];
        int y1 = pred->bbox_int16[jj + 3];

        draw_rect_outline(pixels, x0, y0, x1, y1, overlay_color_by_cid[cid]);
    }
}

static bool encoder_minih264_init(void) {
    int sizeof_persist = 0;
    int sizeof_scratch = 0;
    int status;

    memset(&encoder_create_param, 0, sizeof(encoder_create_param));
    encoder_create_param.width = ENCODER_FRAME_WIDTH;
    encoder_create_param.height = ENCODER_FRAME_HEIGHT;
    encoder_create_param.gop = ENCODER_GOP;
    encoder_create_param.fine_rate_control_flag = 0;
    encoder_create_param.const_input_flag = 1;
    encoder_create_param.max_long_term_reference_frames = 0;
    encoder_create_param.temporal_denoise_flag = 0;
    encoder_create_param.enableNEON = 0;
    encoder_create_param.vbv_size_bytes = (ENCODER_BITRATE_BPS / 8);

    status = H264E_sizeof(&encoder_create_param, &sizeof_persist, &sizeof_scratch);
    if (status != 0) {
        printf("[enc] H264E_sizeof failed: %d\r\n", status);
        return false;
    }
    printf("[enc] persist=%d B (pool=%d B), scratch=%d B (pool=%d B)\r\n",
        sizeof_persist, ENCODER_PERSIST_BYTES,
        sizeof_scratch, ENCODER_SCRATCH_BYTES
    );
    if (sizeof_persist > ENCODER_PERSIST_BYTES ||
        sizeof_scratch > ENCODER_SCRATCH_BYTES) {
        printf("[enc] static pool too small, grow ENCODER_*_BYTES\r\n");
        return false;
    }

    status = H264E_init((H264E_persist_t *)encoder_persist, &encoder_create_param);
    if (status != 0) {
        printf("[enc] H264E_init failed: %d\r\n", status);
        return false;
    }

    encoder_io.yuv[0] = encoder_i420;
    encoder_io.yuv[1] = encoder_i420 + (ENCODER_FRAME_WIDTH * ENCODER_FRAME_HEIGHT);
    encoder_io.yuv[2] = encoder_io.yuv[1] + (ENCODER_FRAME_WIDTH * ENCODER_FRAME_HEIGHT / 4);
    encoder_io.stride[0] = ENCODER_FRAME_WIDTH;
    encoder_io.stride[1] = ENCODER_FRAME_WIDTH / 2;
    encoder_io.stride[2] = ENCODER_FRAME_WIDTH / 2;

    memset(&encoder_run_param, 0, sizeof(encoder_run_param));
    encoder_run_param.frame_type = 0;
    encoder_run_param.encode_speed = H264E_SPEED_FASTEST;
    encoder_run_param.qp_min = 20;
    encoder_run_param.qp_max = 45;
    encoder_run_param.desired_frame_bytes = (ENCODER_BITRATE_BPS / 8) / ENCODER_TARGET_FPS;
    encoder_run_param.desired_nalu_bytes = 0;

    return true;
}

void encoder_task_early_init(void) {
    cy_rslt_t result = cy_rtos_semaphore_init(&encoder_semaphore, 1, 0);
    if (CY_RSLT_SUCCESS != result) {
        CY_ASSERT(0);
    }
    video_ring_init();
}

void encoder_task_start_after_vglite(void) {
    encoder_frame.width = ENCODER_FRAME_WIDTH;
    encoder_frame.height = ENCODER_FRAME_HEIGHT;
    encoder_frame.format = VG_LITE_BGR565;
    encoder_frame.image_mode = VG_LITE_NORMAL_IMAGE_MODE;
    vg_lite_error_t vs = vg_lite_allocate(&encoder_frame);
    if (VG_LITE_SUCCESS != vs) {
        printf("[enc] encoder_frame vg_lite_allocate failed: %d\r\n", vs);
        CY_ASSERT(0);
    }

    vg_lite_identity(&encoder_crop_matrix);
    vg_lite_scale(2.0f, 1.0f, &encoder_crop_matrix);
    vg_lite_translate(-80.0f, 0.0f, &encoder_crop_matrix);

    cy_rslt_t result = cy_rtos_thread_create(
        &encoder_thread, &cm55_encoder_task, ENCODER_TASK_NAME, NULL,
        ENCODER_TASK_STACK_SIZE, ENCODER_TASK_PRIORITY, NULL
    );
    if (CY_RSLT_SUCCESS != result) {
        CY_ASSERT(0);
    }

    encoder_ready = true;
}

void encoder_on_display_frame_done(prediction_od_t *pred) {
    if (!encoder_ready) return;

    encoder_hook_fires++;

    uint32_t now_ms = (uint32_t)ifx_time_get_ms_f();
    if (encoder_input_seen && ((uint32_t)(now_ms - encoder_last_input_ms) < ENCODER_MIN_INPUT_MS)) {
        return;
    }
    encoder_input_seen = true;
    encoder_last_input_ms = now_ms;

    /* Copy camera frame into our owned buffer with a center-160 H crop
     * stretched 2x (matrix built once in encoder_task_start_after_vglite).
     * Softens the OV7675's ~4:1 anamorphic to ~2:1 at the cost of
     * effective H resolution (160 distinct samples spread across 320
     * target px).  FILTER_POINT: nearest-neighbor sampling, cheaper than
     * LINEAR; the visible cost is paired-pixel doubling in H, which the
     * encoder compresses out cleanly.  See work/reference/DVP_ISSUE.md
     * Issue 1. */
    vg_lite_error_t vs = vg_lite_blit(
        &encoder_frame, &bgr565,
        &encoder_crop_matrix, VG_LITE_BLEND_NONE, 0, VG_LITE_FILTER_POINT
    );
    if (VG_LITE_SUCCESS != vs) {
        /* Don't spam UART if blit starts failing; just drop the frame. */
        return;
    }
    vg_lite_finish();

    /* Snapshot predictions under the gfx task so the overlay matches the
     * frame we just copied. */
    if (pred != NULL) {
        encoder_prediction = *pred;
    } else {
        encoder_prediction.count = 0;
    }

    if (encoder_draw_overlays) {
        draw_prediction_overlays(&encoder_frame, &encoder_prediction);
    }

    (void)cy_rtos_semaphore_set(&encoder_semaphore);
}

void cm55_encoder_task(void *arg) {
    CY_UNUSED_PARAMETER(arg);

    if (!encoder_minih264_init()) {
        printf("[enc] minih264 init failed; encoder task idling\r\n");
        for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    printf("[enc] minih264 ready: %dx%d gop=%d bitrate=%d bps\r\n",
        ENCODER_FRAME_WIDTH, ENCODER_FRAME_HEIGHT,
        ENCODER_GOP, ENCODER_BITRATE_BPS
    );

    uint32_t frames = 0;
    uint32_t total_bytes = 0;
    float sum_convert = 0.0f;
    float sum_encode = 0.0f;
    float sum_total = 0.0f;
    float t_prev_report = ifx_time_get_ms_f();

    int max_debug_messages = 2;
    for (;;) {
        cy_rslt_t result = cy_rtos_semaphore_get(&encoder_semaphore, 0xFFFFFFFF);
        if (CY_RSLT_SUCCESS != result) {
            continue;
        }

        float t_start = ifx_time_get_ms_f();

        /* 1. BGR565 -> I420 */
        bgr565_to_i420((const uint16_t *)encoder_frame.memory,
            ENCODER_FRAME_WIDTH, ENCODER_FRAME_HEIGHT,
            encoder_io.yuv[0], encoder_io.yuv[1], encoder_io.yuv[2]);
        float t_after_convert = ifx_time_get_ms_f();

        /* 2. Encode.  frame_type=DEFAULT lets minih264 emit a keyframe at
         * every GOP boundary, including frame 0. */
        unsigned char *coded_data = NULL;
        int            coded_size = 0;
        encoder_run_param.frame_type = H264E_FRAME_TYPE_DEFAULT;

        int status = H264E_encode(
            (H264E_persist_t *)encoder_persist, (H264E_scratch_t *)encoder_scratch,
            &encoder_run_param, &encoder_io, &coded_data, &coded_size
        );
        float t_after_encode = ifx_time_get_ms_f();

        if (status != 0) {
            printf("[enc] H264E_encode failed seq=%u: %d\r\n",
                (unsigned)encoder_frame_seq, status);
            continue;
        }

        /* 3. Publish into the cross-core ring.  Drop-tail: the publish
         * call returns false if CM33 has the session disabled or hasn't
         * released the next slot yet.  Keyframes land at GOP boundaries
         * when frame_type=DEFAULT. */
        bool is_idr = ((encoder_frame_seq % ENCODER_GOP) == 0);
        video_ring_try_publish((const uint8_t *)coded_data, (uint32_t)coded_size,
            (uint32_t)t_start, is_idr, encoder_frame_seq
        );

        encoder_frame_seq++;

        sum_convert += (t_after_convert - t_start);
        sum_encode += (t_after_encode - t_after_convert);
        sum_total += (t_after_encode - t_start);
        total_bytes += (uint32_t)coded_size;
        frames++;

        if (frames >= 30 && max_debug_messages > 0) {
            max_debug_messages--;
            
            float now = ifx_time_get_ms_f();
            float window = now - t_prev_report;
            float fps = (window > 0.0f) ? (1000.0f * frames / window) : 0.0f;

            /* Read-and-clear the producer-side counters atomically enough
             * for our purposes -- a fire that lands between these reads
             * is just credited to the next window. */
            uint32_t hooks = encoder_hook_fires = 0;
            uint32_t inf_done = encoder_inf_done = 0;
            uint32_t dsp_done = encoder_disp_done = 0;

            float in_fps = (window > 0.0f) ? (1000.0f * hooks / window) : 0.0f;
            float inf_fps = (window > 0.0f) ? (1000.0f * inf_done / window) : 0.0f;
            float dsp_fps = (window > 0.0f) ? (1000.0f * dsp_done / window) : 0.0f;
            uint32_t missed = (hooks > frames) ? (hooks - frames) : 0;
            float drop_pct  = (hooks > 0) ? (100.0f * missed / hooks)  : 0.0f;

            const video_ring_header_t         *h  = video_ring_header();
            const video_ring_producer_stats_t *ps = video_ring_producer_stats();
            printf("[enc] inf_fps=%.2f dsp_fps=%.2f in_fps=%.2f enc_fps=%.2f "
                "missed=%u/%u (%.0f%%) conv_ms=%.2f enc_ms=%.2f tot_ms=%.2f "
                "nal_avg=%u idr=%d ring_en=%u pub=%u dbusy=%u doff=%u dovr=%u ovl=%d\r\n",
                (double)inf_fps, (double)dsp_fps, (double)in_fps, (double)fps,
                (unsigned)missed, (unsigned)hooks, (double)drop_pct,
                (double)(sum_convert / frames), (double)(sum_encode / frames), (double)(sum_total / frames),
                (unsigned)(total_bytes / frames), (int)is_idr,
                (unsigned)h->enabled, (unsigned)ps->published,
                (unsigned)ps->dropped_busy, (unsigned)ps->dropped_disabled, (unsigned)ps->dropped_oversize,
                (int)encoder_draw_overlays
            );
            frames = 0;
            total_bytes = 0;
            sum_convert = 0.0f;
            sum_encode = 0.0f;
            sum_total = 0.0f;
            t_prev_report = now;
        }
    }
}
