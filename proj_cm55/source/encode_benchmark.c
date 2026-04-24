/*******************************************************************************
* File Name        : encode_benchmark.c
*
* Description      : M0 spike task. Runs the minih264 software H.264 encoder
*                    on synthetic YUV frames at a few candidate resolutions and
*                    prints per-resolution timing over UART. The output tells
*                    us whether software encoding on the CM55 is viable for a
*                    ~10 fps WebRTC pilot, or whether we fall back to UVC
*                    H.264 passthrough.
*
*                    Synthetic input is used deliberately: this spike measures
*                    CPU cost of the encoder itself, not the camera or USB
*                    pipeline. Those are wired in later milestones.
*
*                    See work/reference/PILOT.md for the full pilot plan.
*
********************************************************************************
 * (c) 2025-2026, Infineon Technologies AG, or an affiliate of Infineon
 * Technologies AG. All rights reserved.
 * This software, associated documentation and materials ("Software") is
 * owned by Infineon Technologies AG or one of its affiliates ("Infineon")
 * and is protected by and subject to worldwide patent protection, worldwide
 * copyright laws, and international treaty provisions. Therefore, you may use
 * this Software only as provided in the license agreement accompanying the
 * software package from which you obtained this Software. If no license
 * agreement applies, then any use, reproduction, modification, translation, or
 * compilation of this Software is prohibited without the express written
 * permission of Infineon.
 *
 * Disclaimer: UNLESS OTHERWISE EXPRESSLY AGREED WITH INFINEON, THIS SOFTWARE
 * IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING, BUT NOT LIMITED TO, ALL WARRANTIES OF NON-INFRINGEMENT OF
 * THIRD-PARTY RIGHTS AND IMPLIED WARRANTIES SUCH AS WARRANTIES OF FITNESS FOR A
 * SPECIFIC USE/PURPOSE OR MERCHANTABILITY.
 * Infineon reserves the right to make changes to the Software without notice.
 * You are responsible for properly designing, programming, and testing the
 * functionality and safety of your intended application of the Software, as
 * well as complying with any legal requirements related to its use. Infineon
 * does not guarantee that the Software will be free from intrusion, data theft
 * or loss, or other breaches ("Security Breaches"), and Infineon shall have
 * no liability arising out of any Security Breaches. Unless otherwise
 * explicitly approved by Infineon, the Software may not be used in any
 * application where a failure of the Product or any consequences of the use
 * thereof can reasonably be expected to result in personal injury.
*******************************************************************************/

/*******************************************************************************
* Header Files
*******************************************************************************/
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "cy_utils.h"
#include "FreeRTOS.h"
#include "task.h"

#include "encode_benchmark.h"

#include "ipc_communication.h"


/* minih264 is distributed as a single-header library. This translation
 * unit owns the implementation. */
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"

/*******************************************************************************
* Macros
*******************************************************************************/
/* Number of encoded frames per resolution. One IDR at frame 0, then inter
 * frames for the rest of the GOP window. 120 frames @ 10 fps targets ~12 s
 * of equivalent real-time video which is enough for a stable average. */
#define ENCODE_BENCHMARK_FRAMES             (120)

/* Target framerate the pilot wants to sustain. Used only to turn the
 * bitrate target into a per-frame byte budget for rate control. */
#define ENCODE_BENCHMARK_TARGET_FPS         (10)

/* GOP length = key-frame period in frames. At 10 fps this places one IDR
 * roughly every 3 s, similar to what WebRTC tends to produce. */
#define ENCODE_BENCHMARK_GOP                (30)

/* Pre-sized static pools. Sized for 640x480 I420 with minih264's plain-C
 * configuration. If H264E_sizeof reports a larger requirement at runtime
 * the task aborts and prints the deficit so we know what to grow. */

// Nik: 640p will need too much. Limit this
#define ENCODE_BENCHMARK_PERSIST_BYTES      (300 * 1024)
#define ENCODE_BENCHMARK_SCRATCH_BYTES      (200 * 1024)
#define ENCODE_BENCHMARK_YUV_BYTES          (320 * 240 * 3 / 2)

/* Rate-control targets. These are conservative placeholders, the pilot
 * tunes them later. */
#define ENCODE_BENCHMARK_BITRATE_QVGA_BPS   (200 * 1000)
#define ENCODE_BENCHMARK_BITRATE_VGA_BPS    (600 * 1000)

/*******************************************************************************
* Types
*******************************************************************************/
typedef struct
{
    const char *label;
    int width;
    int height;
    int bitrate_bps;
} encode_benchmark_case_t;

/*******************************************************************************
* Global Variables
*******************************************************************************/
/* Encoder working memory lives in SOCMEM alongside the ML arena; CM55's
 * default data RAM is too small for the VGA scratch. Same section the
 * inference task and the LCD task already target. */
__attribute__((section(".cy_socmem_data"), aligned(16)))
static uint8_t encode_benchmark_persist[ENCODE_BENCHMARK_PERSIST_BYTES];

__attribute__((section(".cy_socmem_data"), aligned(16)))
static uint8_t encode_benchmark_scratch[ENCODE_BENCHMARK_SCRATCH_BYTES];

__attribute__((section(".cy_socmem_data"), aligned(16)))
static uint8_t encode_benchmark_yuv[ENCODE_BENCHMARK_YUV_BYTES];

/*******************************************************************************
* Function Name: encode_benchmark_fill_synthetic
********************************************************************************
* Summary:
*   Fills the I420 buffer with a moving gradient. The content is not chosen
*   for compressibility; it just needs to vary frame-to-frame so rate control
*   and motion estimation exercise non-trivial code paths.
*
* Parameters:
*   yuv         - planar I420 buffer (Y then U then V, contiguous)
*   width       - luma width in pixels
*   height      - luma height in pixels
*   frame_idx   - frame counter, drives the animation
*
* Return:
*   None
*******************************************************************************/
static void encode_benchmark_fill_synthetic(uint8_t *yuv,
                                            int width,
                                            int height,
                                            int frame_idx)
{
    uint8_t *y_plane = yuv;
    uint8_t *u_plane = yuv + (width * height);
    uint8_t *v_plane = u_plane + (width * height / 4);
    int x;
    int y;

    for (y = 0; y < height; y++)
    {
        for (x = 0; x < width; x++)
        {
            y_plane[(y * width) + x] = (uint8_t)(x + y + frame_idx);
        }
    }

    for (y = 0; y < (height / 2); y++)
    {
        for (x = 0; x < (width / 2); x++)
        {
            u_plane[(y * (width / 2)) + x] = (uint8_t)(128 + x - frame_idx);
            v_plane[(y * (width / 2)) + x] = (uint8_t)(128 + y + frame_idx);
        }
    }
}

/*******************************************************************************
* Function Name: encode_benchmark_run_case
********************************************************************************
* Summary:
*   Initializes the encoder for a single resolution, encodes
*   ENCODE_BENCHMARK_FRAMES synthetic frames, and prints the average
*   per-frame encode time plus derived FPS and NAL size over UART.
*
* Parameters:
*   test_case - which resolution and bitrate to exercise
*
* Return:
*   None
*******************************************************************************/
static void encode_benchmark_run_case(const encode_benchmark_case_t *test_case)
{
    H264E_create_param_t create_param;
    H264E_run_param_t    run_param;
    H264E_io_yuv_t       frame;
    int                  sizeof_persist = 0;
    int                  sizeof_scratch = 0;
    int                  status;
    int                  frame_idx;
    unsigned char       *coded_data = NULL;
    int                  coded_size = 0;
    uint64_t             total_bytes = 0;
    TickType_t           t_start;
    TickType_t           t_end;
    uint32_t             elapsed_ms;
    float                avg_ms_per_frame;
    float                measured_fps;

    printf("\r\n[enc] --- %s %dx%d @ %d bps ---\r\n",
           test_case->label,
           test_case->width,
           test_case->height,
           test_case->bitrate_bps);

    memset(&create_param, 0, sizeof(create_param));
    create_param.width  = test_case->width;
    create_param.height = test_case->height;
    create_param.gop    = ENCODE_BENCHMARK_GOP;
    create_param.fine_rate_control_flag = 0;
    create_param.const_input_flag       = 1;
    create_param.max_long_term_reference_frames = 0;
    create_param.temporal_denoise_flag  = 0;
    create_param.enableNEON             = 0;
    create_param.vbv_size_bytes         = (test_case->bitrate_bps / 8);

    status = H264E_sizeof(&create_param, &sizeof_persist, &sizeof_scratch);
    if (0 != status)
    {
        printf("[enc] H264E_sizeof failed: %d\r\n", status);
        return;
    }

    printf("[enc] persist=%d B (pool=%d B), scratch=%d B (pool=%d B)\r\n",
           sizeof_persist, ENCODE_BENCHMARK_PERSIST_BYTES,
           sizeof_scratch, ENCODE_BENCHMARK_SCRATCH_BYTES);

    if ((sizeof_persist > ENCODE_BENCHMARK_PERSIST_BYTES) ||
        (sizeof_scratch > ENCODE_BENCHMARK_SCRATCH_BYTES))
    {
        printf("[enc] ERROR: static pool too small, grow the pool\r\n");
        return;
    }

    status = H264E_init((H264E_persist_t *)encode_benchmark_persist, &create_param);
    if (0 != status)
    {
        printf("[enc] H264E_init failed: %d\r\n", status);
        return;
    }

    frame.yuv[0] = encode_benchmark_yuv;
    frame.yuv[1] = encode_benchmark_yuv + (test_case->width * test_case->height);
    frame.yuv[2] = frame.yuv[1] + (test_case->width * test_case->height / 4);
    frame.stride[0] = test_case->width;
    frame.stride[1] = test_case->width / 2;
    frame.stride[2] = test_case->width / 2;

    memset(&run_param, 0, sizeof(run_param));
    run_param.frame_type         = 0;
    run_param.encode_speed       = H264E_SPEED_FASTEST;
    run_param.qp_min             = 20;
    run_param.qp_max             = 45;
    run_param.desired_frame_bytes =
        (test_case->bitrate_bps / 8) / ENCODE_BENCHMARK_TARGET_FPS;
    run_param.desired_nalu_bytes = 0;

    t_start = xTaskGetTickCount();

    for (frame_idx = 0; frame_idx < ENCODE_BENCHMARK_FRAMES; frame_idx++)
    {
        encode_benchmark_fill_synthetic(encode_benchmark_yuv,
                                        test_case->width,
                                        test_case->height,
                                        frame_idx);

        status = H264E_encode((H264E_persist_t *)encode_benchmark_persist,
                              (H264E_scratch_t *)encode_benchmark_scratch,
                              &run_param,
                              &frame,
                              &coded_data,
                              &coded_size);
        if (0 != status)
        {
            printf("[enc] H264E_encode failed frame %d: %d\r\n", frame_idx, status);
            return;
        }
        total_bytes += (uint64_t)coded_size;
        taskYIELD(); // yield after every frame to see if inference will starve us
    }

    t_end = xTaskGetTickCount();

    elapsed_ms       = (uint32_t)((t_end - t_start) * portTICK_PERIOD_MS);
    avg_ms_per_frame = (float)elapsed_ms / (float)ENCODE_BENCHMARK_FRAMES;
    measured_fps     = (avg_ms_per_frame > 0.0f)
                       ? (1000.0f / avg_ms_per_frame)
                       : 0.0f;

    printf("[enc] res=%dx%d frames=%d total_ms=%u avg_ms=%.2f fps=%.2f "
           "nal_avg=%u B\r\n",
           test_case->width,
           test_case->height,
           ENCODE_BENCHMARK_FRAMES,
           (unsigned)elapsed_ms,
           (double)avg_ms_per_frame,
           (double)measured_fps,
           (unsigned)(total_bytes / (uint64_t)ENCODE_BENCHMARK_FRAMES));
}

/*******************************************************************************
* Function Name: cm55_encode_benchmark_task
********************************************************************************
* Summary:
*   FreeRTOS entry point for the M0 spike. Runs each configured case once and
*   then idles the task. Output is UART only; no LCD, no network, no camera.
*
* Parameters:
*   arg - unused
*
* Return:
*   None
*******************************************************************************/
void cm55_encode_benchmark_task(void *arg)
{
    const encode_benchmark_case_t encode_benchmark_cases[] =
    {
        { "QVGA", 320, 240, ENCODE_BENCHMARK_BITRATE_QVGA_BPS },
        // Nik: this won't fit I think
        // { "VGA",  640, 480, ENCODE_BENCHMARK_BITRATE_VGA_BPS  },
    };


    CY_UNUSED_PARAMETER(arg);

    printf("\r\n[enc] === M0: minih264 software encode benchmark ===\r\n");
    printf("[enc] target_fps=%d gop=%d frames_per_case=%d\r\n",
           ENCODE_BENCHMARK_TARGET_FPS,
           ENCODE_BENCHMARK_GOP,
           ENCODE_BENCHMARK_FRAMES);

    size_t idx;
    for (idx = 0; idx < (sizeof(encode_benchmark_cases) /
                         sizeof(encode_benchmark_cases[0])); idx++)
    {
        encode_benchmark_run_case(&encode_benchmark_cases[idx]);
    }

    printf("\r\n[enc] === benchmark complete ===\r\n");

    int j = 0;
    for (;;)
    {        
        ipc_payload_t* payload = cm55_ipc_get_payload_ptr();
        payload->label_id = 0;
        cm55_ipc_send_to_cm33();
        strcpy(payload->label, "idle");
        vTaskDelay(pdMS_TO_TICKS(10000));
        j=(j+1) % 10; // just to track changes
    }
}

/* [] END OF FILE */
