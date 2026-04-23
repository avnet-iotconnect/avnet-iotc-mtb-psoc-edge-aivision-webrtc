/*******************************************************************************
* File Name        : encode_benchmark.h
*
* Description      : M0 spike for the WebRTC pilot. Measures how many frames
*                    per second the CM55 core can produce with the minih264
*                    software H.264 encoder for the pilot's candidate
*                    resolutions. Results gate whether we pursue software
*                    encoding (M6) or fall back to UVC H.264 passthrough
*                    (M6-alt).
*
*                    See work/reference/PILOT.md for the full pilot plan.
*
* Related Document : See README.md
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

#ifndef ENCODE_BENCHMARK_H_
#define ENCODE_BENCHMARK_H_

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
* Header Files
*******************************************************************************/
#include "FreeRTOS.h"
#include "task.h"

/*******************************************************************************
* Macros
*******************************************************************************/
#define ENCODE_BENCHMARK_TASK_NAME          ( "CM55 Encode Benchmark" )
#define ENCODE_BENCHMARK_TASK_STACK_SIZE    ( 8U * 1024U )
#define ENCODE_BENCHMARK_TASK_PRIORITY      ( configMAX_PRIORITIES - 4 )

/*******************************************************************************
* Function Prototypes
*******************************************************************************/
void cm55_encode_benchmark_task(void *arg);

#if defined(__cplusplus)
}
#endif

#endif /* ENCODE_BENCHMARK_H_ */

/* [] END OF FILE */
