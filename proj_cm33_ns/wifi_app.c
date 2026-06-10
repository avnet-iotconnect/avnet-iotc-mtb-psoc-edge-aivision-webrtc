/******************************************************************************
* File Name:   wifi_app.c
*
* Description: This file contains the task that handles initialization &
*              connection of Wi-Fi and the MQTT client. The task then starts
*              the subscriber and the publisher tasks. The task also implements
*              reconnection mechanisms to handle WiFi and MQTT disconnections.
*              The task also handles all the cleanup operations to gracefully
*              terminate the Wi-Fi and MQTT connections in case of any failure.
*
* Related Document: See README.md
*
*
*******************************************************************************
* * Copyright 2024-2025, Cypress Semiconductor Corporation (an Infineon company) or
* * an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
* *
* * This software, including source code, documentation and related
* * materials ("Software") is owned by Cypress Semiconductor Corporation
* * or one of its affiliates ("Cypress") and is protected by and subject to
* * worldwide patent protection (United States and foreign),
* * United States copyright laws and international treaty provisions.
* * Therefore, you may use this Software only as provided in the license
* * agreement accompanying the software package from which you
* * obtained this Software ("EULA").
* * If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* * non-transferable license to copy, modify, and compile the Software
* * source code solely for use in connection with Cypress's
* * integrated circuit products.  Any reproduction, modification, translation,
* * compilation, or representation of this Software except as specified
* * above is prohibited without the express written permission of Cypress.
* *
* * Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* * EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* * reserves the right to make changes to the Software without notice. Cypress
* * does not assume any liability arising out of the application or use of the
* * Software or any product or circuit described in the Software. Cypress does
* * not authorize its products for use in any products where a malfunction or
* * failure of the Cypress product may reasonably be expected to result in
* * significant property damage, injury or death ("High Risk Product"). By
* * including Cypress's product in a High Risk Product, the manufacturer
* * of such system or application assumes all risk of such use and in doing
* * so agrees to indemnify Cypress against all liability.
*******************************************************************************/
/* SPDX-License-Identifier: MIT
 * Copyright (C) 2025 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com>, Shu Liu <shu.liu@avnet.com> et al.
 */

#include "cybsp.h"

/* FreeRTOS header files */
#include "FreeRTOS.h"
#include "task.h"

/* LwIP header files */
#include "lwip/netif.h"

/* Middleware libraries */
#include "cy_wcm.h"
#include "retarget_io_init.h"

#include "wifi_config.h"

/******************************************************************************
* Macros
******************************************************************************/

/* Flag Masks for tracking which cleanup functions must be called. */
#define WCM_INITIALIZED                             (1lu << 0)
#define WIFI_CONNECTED                              (1lu << 1)
#define TIME_DIV_MS                                 (60000u)
#define APP_SDIO_INTERRUPT_PRIORITY                 (7U)
#define APP_HOST_WAKE_INTERRUPT_PRIORITY            (2U)
#define APP_SDIO_FREQUENCY_HZ                       (25000000U)
#define SDHC_SDIO_64BYTES_BLOCK                     (64U)


/* Macro to check if the result of an operation was successful and set the
 * corresponding bit in the status_flag based on 'init_mask' parameter. When
 * it has failed, print the error message and return the result to the
 * calling function.
 */
#define CHECK_RESULT(result, init_mask, error_message...)      \
                     do                                        \
                     {                                         \
                         if ((int)result == CY_RSLT_SUCCESS)   \
                         {                                     \
                             status_flag |= init_mask;         \
                         }                                     \
                         else                                  \
                         {                                     \
                             printf(error_message);            \
                             return result;                    \
                         }                                     \
                     } while(0)
/******************************************************************************
* Local Variables
*******************************************************************************/
/* Flag to denote initialization status of various operations. */
static uint32_t status_flag;

/* Pointer to the network buffer needed by the MQTT library for MQTT send and
 * receive operations.
 */
static mtb_hal_sdio_t sdio_instance;
static cy_stc_sd_host_context_t sdhc_host_context;
static cy_wcm_config_t wcm_config;


/******************************************************************************
 * Function Name: wifi_connect
 ******************************************************************************
 * Summary:
 *  Function that initiates connection to the Wi-Fi Access Point using the
 *  specified SSID and PASSWORD. The connection is retried a maximum of
 *  'MAX_WIFI_CONN_RETRIES' times with interval of 'WIFI_CONN_RETRY_INTERVAL_MS'
 *  milliseconds.
 *
 * Parameters:
 *  void
 *
 * Return:
 *  cy_rslt_t : CY_RSLT_SUCCESS upon a successful Wi-Fi connection, else an
 *              error code indicating the failure.
 *
 ******************************************************************************/
static cy_rslt_t wifi_connect(const char* ssid, const char* password)
{
    cy_rslt_t result = CY_RSLT_SUCCESS;
    cy_wcm_connect_params_t connect_param;
    cy_wcm_ip_address_t ip_address;

    /* Check if Wi-Fi connection is already established. */
    if (!(cy_wcm_is_connected_to_ap()))
    {
        /* Configure the connection parameters for the Wi-Fi interface. */
        memset(&connect_param, 0, sizeof(cy_wcm_connect_params_t));
        strcpy((char * ) connect_param.ap_credentials.SSID, ssid);
        if (NULL != password) {
            strcpy((char * ) connect_param.ap_credentials.password, password);
        } else {
            // NOTE: Untested for open networks
            printf("Open network specified. Attempting to connect without a password.\n");
        }
        connect_param.ap_credentials.security = WIFI_SECURITY;

        printf("\nWi-Fi Connecting to '%s'\n", connect_param.ap_credentials.SSID);

        /* Connect to the Wi-Fi AP. */
        for (uint32_t retry_count = 0; retry_count < MAX_WIFI_CONN_RETRIES; retry_count++)
        {
            result = cy_wcm_connect_ap(&connect_param, &ip_address);

            if (CY_RSLT_SUCCESS == result)
            {
                printf("Successfully connected to Wi-Fi network '%s'.\n", connect_param.ap_credentials.SSID);

                /* Set the appropriate bit in the status_flag to denote
                 * successful Wi-Fi connection, print the assigned IP address.
                 */
                status_flag |= WIFI_CONNECTED;
                if (ip_address.version == CY_WCM_IP_VER_V4)
                {
                    printf("IPv4 Address Assigned: %s\n\n", ip4addr_ntoa((const ip4_addr_t *) &ip_address.ip.v4));
                }
                else if (ip_address.version == CY_WCM_IP_VER_V6)
                {
                    printf("IPv6 Address Assigned: %s\n\n", ip6addr_ntoa((const ip6_addr_t *) &ip_address.ip.v6));
                }
                return result;
            }

            printf("Wi-Fi Connection failed. Error code:0x%0X. Retrying in %d ms. Retries left: %d\n",
                (int)result, WIFI_CONN_RETRY_INTERVAL_MS, (int)(MAX_WIFI_CONN_RETRIES - retry_count - 1));
            vTaskDelay(pdMS_TO_TICKS(WIFI_CONN_RETRY_INTERVAL_MS));
        }

        printf("\nExceeded maximum Wi-Fi connection attempts!\n");
        printf("Wi-Fi connection failed after retrying for %d mins\n\n",
            (int)(WIFI_CONN_RETRY_INTERVAL_MS * MAX_WIFI_CONN_RETRIES) / TIME_DIV_MS);
    }
    return result;
}


/*******************************************************************************
* Function Name: sdio_interrupt_handler
********************************************************************************
* Summary:
* Interrupt handler function for SDIO instance.
*******************************************************************************/
static void sdio_interrupt_handler(void)
{
    mtb_hal_sdio_process_interrupt(&sdio_instance);
}

/*******************************************************************************
* Function Name: host_wake_interrupt_handler
********************************************************************************
* Summary:
* Interrupt handler function for the host wake up input pin.
*******************************************************************************/
static void host_wake_interrupt_handler(void)
{
    mtb_hal_gpio_process_interrupt(&wcm_config.wifi_host_wake_pin);
}

/*******************************************************************************
* Function Name: app_sdio_init
********************************************************************************
* Summary:
* This function configures and initializes the SDIO instance used in
* communication between the host MCU and the wireless device.
*******************************************************************************/
static void app_sdio_init(void)
{
    cy_rslt_t result;
    mtb_hal_sdio_cfg_t sdio_hal_cfg;
    cy_stc_sysint_t sdio_intr_cfg =
    {
        .intrSrc = CYBSP_WIFI_SDIO_IRQ,
        .intrPriority = APP_SDIO_INTERRUPT_PRIORITY
    };

    cy_stc_sysint_t host_wake_intr_cfg =
    {
            .intrSrc = CYBSP_WIFI_HOST_WAKE_IRQ,
            .intrPriority = APP_HOST_WAKE_INTERRUPT_PRIORITY
    };

    /* Initialize the SDIO interrupt and specify the interrupt handler. */
    cy_en_sysint_status_t interrupt_init_status = Cy_SysInt_Init(&sdio_intr_cfg, sdio_interrupt_handler);

    /* SDIO interrupt initialization failed. Stop program execution. */
    if(CY_SYSINT_SUCCESS != interrupt_init_status)
    {
        handle_app_error();
    }

    /* Enable NVIC interrupt. */
    NVIC_EnableIRQ(CYBSP_WIFI_SDIO_IRQ);

    /* Setup SDIO using the HAL object and desired configuration */
    result = mtb_hal_sdio_setup(&sdio_instance, &CYBSP_WIFI_SDIO_sdio_hal_config, NULL, &sdhc_host_context);

    /* SDIO setup failed. Stop program execution. */
    if(CY_RSLT_SUCCESS != result)
    {
        handle_app_error();
    }

    /* Initialize and Enable SD HOST */
    Cy_SD_Host_Enable(CYBSP_WIFI_SDIO_HW);
    Cy_SD_Host_Init(CYBSP_WIFI_SDIO_HW, CYBSP_WIFI_SDIO_sdio_hal_config.host_config, &sdhc_host_context);
    Cy_SD_Host_SetHostBusWidth(CYBSP_WIFI_SDIO_HW, CY_SD_HOST_BUS_WIDTH_4_BIT);

    sdio_hal_cfg.frequencyhal_hz = APP_SDIO_FREQUENCY_HZ;
    sdio_hal_cfg.block_size = SDHC_SDIO_64BYTES_BLOCK;

    /* Configure SDIO */
    mtb_hal_sdio_configure(&sdio_instance, &sdio_hal_cfg);

    /* Setup GPIO using the HAL object for WIFI WL REG ON  */
    mtb_hal_gpio_setup(&wcm_config.wifi_wl_pin, CYBSP_WIFI_WL_REG_ON_PORT_NUM, CYBSP_WIFI_WL_REG_ON_PIN);

    /* Setup GPIO using the HAL object for WIFI HOST WAKE PIN  */
    mtb_hal_gpio_setup(&wcm_config.wifi_host_wake_pin, CYBSP_WIFI_HOST_WAKE_PORT_NUM, CYBSP_WIFI_HOST_WAKE_PIN);

    /* Initialize the Host wakeup interrupt and specify the interrupt handler. */
    cy_en_sysint_status_t interrupt_init_status_host_wake =  Cy_SysInt_Init(&host_wake_intr_cfg, host_wake_interrupt_handler);

    /* Host wake up interrupt initialization failed. Stop program execution. */
    if(CY_SYSINT_SUCCESS != interrupt_init_status_host_wake)
    {
        handle_app_error();
    }

    /* Enable NVIC interrupt. */
    NVIC_EnableIRQ(CYBSP_WIFI_HOST_WAKE_IRQ);
}

// This call will not return if it fails
void wifi_app_connect(const char* ssid, const char* password) {

    // psoc edge  needs this for WiFi
    app_sdio_init();

    /* Configure the Wi-Fi interface as a Wi-Fi STA (i.e. Client). */
    wcm_config.interface = CY_WCM_INTERFACE_TYPE_STA;
    wcm_config.wifi_interface_instance = &sdio_instance;

    /* Initialize the Wi-Fi Connection Manager and jump to the cleanup block
     * upon failure.
     */
    if (CY_RSLT_SUCCESS != cy_wcm_init(&wcm_config)) {
        printf("Failed to intialize the WiFi interface.\n");
        while (1) { taskYIELD(); }
    }
    status_flag |= WCM_INITIALIZED;

    /* Set the appropriate bit in the status_flag to denote successful
     * WCM initialization.
     */
    printf("Wi-Fi Connection Manager initialized.\n");
    if (NULL == ssid) {
        printf("Wi-Fi SSID and Password must be provided.\n");
        while (1) { taskYIELD(); }
    }

    /* Initiate connection to the Wi-Fi AP and cleanup if the operation fails. */
    if (CY_RSLT_SUCCESS == wifi_connect(ssid, password)) {
		printf("wifi is connected.\n");
	} else {
		if (CY_RSLT_SUCCESS != wifi_connect(ssid, password)) {
			printf("wifi failed to connect.\n");
			while (1) { taskYIELD(); }
		}
	}
}

/* [] END OF FILE */
