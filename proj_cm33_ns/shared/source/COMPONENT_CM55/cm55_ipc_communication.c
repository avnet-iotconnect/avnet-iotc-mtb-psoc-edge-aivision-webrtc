/*******************************************************************************
* File Name        : main.c
*
* Description      : This source file contains the code for setting up IPC 
*                    communication for CM55 CPU.
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

#include "ipc_communication.h"

/*******************************************************************************
* Global Variable(s)
*******************************************************************************/
/* Create an array of endpoint structures */
static cy_stc_ipc_pipe_ep_t cm55_ipc_pipe_array[CY_IPC_MAX_ENDPOINTS];

/* CB Array for EP2 */
static cy_ipc_pipe_callback_ptr_t ep2_cb_array[CY_IPC_CYPIPE_CLIENT_CNT];

CY_SECTION_SHAREDMEM static ipc_msg_t cm55_msg_data;


__STATIC_INLINE void handle_app_error(void)
{
    /* Disable all interrupts. */
    __disable_irq();

    CY_ASSERT(0);

    /* Infinite loop */
    while(true);

}

/*******************************************************************************
* Function Name: Cy_SysIpcPipeIsrCm55
********************************************************************************
* Summary: 
* This is the interrupt service routine for the system pipe.
*
* Parameters:
*  none
*
* Return :
*  void
*
*******************************************************************************/
void Cy_SysIpcPipeIsrCm55(void)
{
    Cy_IPC_Pipe_ExecuteCallback(CM55_IPC_PIPE_EP_ADDR);
}


/*******************************************************************************
* Function Name: cm55_ipc_communication_setup
********************************************************************************
* Summary:
*  This function configures IPC Pipe for CM55 to CM33 communication.
*
* Parameters:
*  none
*
* Return :
*  void
*
*******************************************************************************/
void cm55_ipc_communication_setup(void)
{

    /* IPC pipe endpoint-1 and endpoint-2. CM55 <--> CM33 */
    static const cy_stc_ipc_pipe_config_t cm55_ipc_pipe_config =
    {
        /* receiver endpoint CM55 */
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
        /* sender endpoint CM33 */
        {
            .ipcNotifierNumber     = CY_IPC_INTR_CYPIPE_EP1,
            .ipcNotifierPriority   = CY_IPC_INTR_CYPIPE_PRIOR_EP1,
            .ipcNotifierMuxNumber  = CY_IPC_INTR_CYPIPE_MUX_EP1,
            .epAddress             = CM33_IPC_PIPE_EP_ADDR,
            {
                .epChannel         = CY_IPC_CHAN_CYPIPE_EP1,
                .epIntr            = CY_IPC_INTR_CYPIPE_EP1,
                .epIntrmask        = CY_IPC_CYPIPE_INTR_MASK
            }
        },
    .endpointClientsCount          = CY_IPC_CYPIPE_CLIENT_CNT,
    .endpointsCallbacksArray       = ep2_cb_array,
    .userPipeIsrHandler            = &Cy_SysIpcPipeIsrCm55
    };

    Cy_IPC_Pipe_Config(cm55_ipc_pipe_array);

    Cy_IPC_Pipe_Init(&cm55_ipc_pipe_config);
}


ipc_payload_t* cm55_ipc_get_payload_ptr(void)
{
    return &cm55_msg_data.payload;
}

void cm55_ipc_send_to_cm33(void)
{
    cy_en_ipc_pipe_status_t pipe_status;

    cm55_msg_data.client_id = CM33_IPC_PIPE_CLIENT_ID;
    cm55_msg_data.intr_mask = CY_IPC_CYPIPE_INTR_MASK_EP2;

    pipe_status = Cy_IPC_Pipe_SendMessage(CM33_IPC_PIPE_EP_ADDR,
                             CM55_IPC_PIPE_EP_ADDR,
                             (void *) &cm55_msg_data, 0);
    if (CY_IPC_PIPE_SUCCESS != pipe_status) {
        handle_app_error();
    }
}