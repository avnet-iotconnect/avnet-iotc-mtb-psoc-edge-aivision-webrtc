/*******************************************************************************
* File Name        : main.c
*
* Description      : This source file contains the code for setting up IPC 
*                    communication for CM33 CPU.
*
* Related Document : See README.md
*
********************************************************************************
* Copyright 2025, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/
/* SPDX-License-Identifier: MIT
 * Copyright (C) 2025 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com>, Shu Liu <shu.liu@avnet.com> et al.
 */

#include <string.h>
#include "cybsp.h"
#include "FreeRTOS.h"
#include "retarget_io_init.h"
#include "ipc_communication.h"


/*******************************************************************************
* Global Variable(s)
*******************************************************************************/
/* Create an array of endpoint structures */
static cy_stc_ipc_pipe_ep_t cm33_ipc_pipe_ep_array[CY_IPC_MAX_ENDPOINTS];

/* CB Array for EP1 */
static cy_ipc_pipe_callback_ptr_t ep1_cb_array[CY_IPC_CYPIPE_CLIENT_CNT]; 

/* Allocate and initialize semaphores for the system operations. */
CY_SECTION_SHAREDMEM
static uint32_t ipc_sema_array[CY_IPC_SEMA_COUNT / CY_IPC_SEMA_PER_WORD];

/* Local copy of the IPC message received
   This copy is not safe to be accessed from a task.
   A task must make its own copy of this structure
   while guarding the copying with taskENTER_CRITICAL() and taskEXIT_CRITICAL()
*/
static ipc_msg_t ipc_recv_msg = {0};
static ipc_payload_t ipc_last_detection_payload = {0};
static bool ipc_has_saved_detection = false; // will be set upon receipt. reset when value is checked
static bool ipc_has_received_message = false; // will be set upon receipt. reset when value is checked


/*******************************************************************************
* Function Name: cm33_ipc_pipe_isr
********************************************************************************
* Callback for receipt of message from cm55
*******************************************************************************/
static void cm33_msg_callback(uint32_t * msg_data)
{
    if (msg_data != NULL) {
        /* Copy the message received into our own copy IPC structure */
        memcpy(&ipc_recv_msg, (void *) msg_data, sizeof(ipc_recv_msg));
        if (ipc_recv_msg.payload.label_id != 0) {
            memcpy(&ipc_last_detection_payload, &ipc_recv_msg.payload, sizeof(ipc_payload_t));
            ipc_has_saved_detection = true;
        }
        ipc_has_received_message = true;
    }
}

/*******************************************************************************
* Function Name: cm33_ipc_pipe_isr
********************************************************************************
*
* This is the interrupt service routine for the system pipe.
*
* Parameters:
*  none
*
* Return :
*  void
*
*******************************************************************************/
void cm33_ipc_pipe_isr(void)
{
    Cy_IPC_Pipe_ExecuteCallback(CM33_IPC_PIPE_EP_ADDR);
}

/*******************************************************************************
* Function Name: cm33_ipc_communication_setup
********************************************************************************
* Summary:
* This function...
* 1. Initializes IPC Semaphore.
* 2. Configures IPC Pipe for CM33 to CM55 communication.
*
* Parameters:
*  none
*
* Return :
*  void
*
*******************************************************************************/
void cm33_ipc_communication_setup(void)
{
    /* IPC pipe endpoint-1 and endpoint-2. CM33 <--> CM55 */
    static const cy_stc_ipc_pipe_config_t cm33_ipc_pipe_config =
    {
        /* receiver endpoint CM33 */
        {
            .ipcNotifierNumber    = CY_IPC_INTR_CYPIPE_EP1,
            .ipcNotifierPriority  = CY_IPC_INTR_CYPIPE_PRIOR_EP1,
            .ipcNotifierMuxNumber = CY_IPC_INTR_CYPIPE_MUX_EP1,
            .epAddress            = CM33_IPC_PIPE_EP_ADDR,
            {
                .epChannel        = CY_IPC_CHAN_CYPIPE_EP1,
                .epIntr           = CY_IPC_INTR_CYPIPE_EP1,
                .epIntrmask       = CY_IPC_CYPIPE_INTR_MASK
            }
        },
        /* sender endpoint CM55 */
        {
            .ipcNotifierNumber     = CY_IPC_INTR_CYPIPE_EP2,
            .ipcNotifierPriority   = CY_IPC_INTR_CYPIPE_PRIOR_EP2,
            .ipcNotifierMuxNumber  = CY_IPC_INTR_CYPIPE_MUX_EP2,
            .epAddress             = CM55_IPC_PIPE_EP_ADDR,
            {
                .epChannel         = CY_IPC_CHAN_CYPIPE_EP2,
                .epIntr            = CY_IPC_INTR_CYPIPE_EP2,
                .epIntrmask        = CY_IPC_CYPIPE_INTR_MASK
            }
        },
    .endpointClientsCount          = CY_IPC_CYPIPE_CLIENT_CNT,
    .endpointsCallbacksArray       = ep1_cb_array,
    .userPipeIsrHandler            = &cm33_ipc_pipe_isr
    };

    Cy_IPC_Sema_Init(IPC0_SEMA_CH_NUM, CY_IPC_SEMA_COUNT, ipc_sema_array);

    Cy_IPC_Pipe_Config(cm33_ipc_pipe_ep_array);

    Cy_IPC_Pipe_Init(&cm33_ipc_pipe_config);

    cy_en_ipc_pipe_status_t pipe_status;
    /* Register a callback function to handle events on the CM33 IPC pipe */
    pipe_status = Cy_IPC_Pipe_RegisterCallback(CM33_IPC_PIPE_EP_ADDR, &cm33_msg_callback,
                                              (uint32_t)CM33_IPC_PIPE_CLIENT_ID);
    if (CY_IPC_PIPE_SUCCESS != pipe_status) {
        handle_app_error();
    }

}

bool cm33_ipc_has_received_message(void)
{
    taskENTER_CRITICAL();
    bool ret = ipc_has_received_message;
    ipc_has_received_message = false;
    taskEXIT_CRITICAL();
    return ret;
}

void cm33_ipc_safe_copy_last_payload(ipc_payload_t* target)
{
    taskENTER_CRITICAL();
    memcpy(target, &ipc_recv_msg.payload, sizeof(ipc_payload_t));
    taskEXIT_CRITICAL();
}

bool cm33_ipc_safe_get_and_clear_cached_detection(ipc_payload_t* target)
{
    taskENTER_CRITICAL();
    if (ipc_has_saved_detection) {
        memcpy(target, &ipc_last_detection_payload, sizeof(ipc_payload_t));
        ipc_has_saved_detection = false;
        taskEXIT_CRITICAL();
        return true;
    } else { 
        // else use the last payload - it will not have a detection
        memcpy(target, &ipc_recv_msg.payload, sizeof(ipc_payload_t));
        taskEXIT_CRITICAL();
        return false;
    }
}
