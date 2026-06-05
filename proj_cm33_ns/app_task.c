/* SPDX-License-Identifier: MIT
 * Copyright (C) 2025 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com>, Shu Liu <shu.liu@avnet.com> et al.
 */

#include "cybsp.h"
#include <string.h>

#include "cy_syslib.h" // for Cy_SysLib_GetUniqueId

#include "FreeRTOS.h"

#include "retarget_io_init.h"
#include "ipc_communication.h"

#include "wifi_config.h"
#include "wifi_app.h"

#include "iotconnect.h"
#include "iotc_gencert.h"
#include "iotc_mtb_time.h"
#include "app_eeprom_data.h"

#include "app_config.h"
#include "app_webrtc.h"

#include <inttypes.h>

/////////////////////////////////////////////////////////////////////////////

#define APP_VERSION "1.0.0"

typedef enum UserInputYnStatus {
	APP_INPUT_NONE = 0,
	APP_INPUT_YES,
	APP_INPUT_NO
} UserInputYnStatus;

static UserInputYnStatus user_input_status = APP_INPUT_NONE;

static bool is_demo_mode = false;
static int reporting_interval = 2000;

/////////////////////////////////////////////////////////////////////////////

static void on_connection_status(IotConnectConnectionStatus status) {
    // Add your own status handling
    switch (status) {
        case IOTC_CS_MQTT_CONNECTED:
            printf("/IOTCONNECT Client Connected notification.\n");
            break;
        case IOTC_CS_MQTT_DISCONNECTED:
            printf("/IOTCONNECT Client Disconnected notification.\n");
            break;
        default:
            printf("/IOTCONNECT Client ERROR notification\n");
            break;
    }
}

static void on_ota(IotclC2dEventData data) {
    const char *ota_host = iotcl_c2d_get_ota_url_hostname(data, 0);
    if (ota_host == NULL) {
        printf("OTA host is invalid.\n");
        return;
    }
    const char *ota_path = iotcl_c2d_get_ota_url_resource(data, 0);
    if (ota_path == NULL) {
        printf("OTA resource is invalid.\n");
        return;
    }
    printf("OTA download request received for https://%s%s, but it is not implemented.\n", ota_host, ota_path);
}

// returns success on matching the expected format. Returns is_on, assuming "on" for true, "off" for false
static bool parse_on_off_command(const char* command, const char* name, bool *arg_parsing_success, bool *is_on, const char** message) {
    *arg_parsing_success = false;
    *message = NULL;
    size_t name_len = strlen(name);
    if (0 == strncmp(command, name, name_len)) {
        if (strlen(command) < name_len + 2) { // one for space and at least one character for the argument
            printf("ERROR: Expected command \"%s\" to have an argument\n", command);
            *message = "Command requires an argument";
            *arg_parsing_success = false;
        } else if (0 == strcmp(&command[name_len + 1], "on")) {
            *is_on = true;
            *message = "Value is now \"on\"";
            *arg_parsing_success = true;
        } else if (0 == strcmp(&command[name_len + 1], "off")) {
            *is_on = false;
            *message = "Value is now \"off\"";
            *arg_parsing_success = true;
        } else {
            *message = "Command argument";
            *arg_parsing_success = false;
        }
        // we matches the command
        return true;
    }

    // not our command
    return false;
}

static void on_command(IotclC2dEventData data) {
    const char * const BOARD_STATUS_LED = "board-user-led";
    const char * const DEMO_MODE_CMD = "demo-mode";
    const char * const SET_REPORTING_INTERVAL = "set-reporting-interval "; // with a space

    bool command_success = false;
    const char * message = NULL;

    const char *command = iotcl_c2d_get_command(data);
    const char *ack_id = iotcl_c2d_get_ack_id(data);

    if (command) {
        bool arg_parsing_success;
        printf("Command %s received with %s ACK ID\n", command, ack_id ? ack_id : "no");
        // could be a command without acknowledgment, so ackID can be null
        bool led_on;
        if (parse_on_off_command(command, BOARD_STATUS_LED, &arg_parsing_success, &led_on, &message)) {
            command_success = arg_parsing_success;
            if (arg_parsing_success) {
                if (led_on) {
                    Cy_GPIO_Set(CYBSP_USER_LED_PORT, CYBSP_USER_LED_PIN);
                } else {
                    Cy_GPIO_Clr(CYBSP_USER_LED_PORT, CYBSP_USER_LED_PIN);
                }
            }
        } else if (parse_on_off_command(command, DEMO_MODE_CMD,  &arg_parsing_success, &is_demo_mode, &message)) {
            command_success = arg_parsing_success;
        } else if (0 == strncmp(SET_REPORTING_INTERVAL, command, strlen(SET_REPORTING_INTERVAL))) {
        	int value = atoi(&command[strlen(SET_REPORTING_INTERVAL)]);
        	if (0 == value) {
                message = "Argument parsing error";
        	} else {
        		reporting_interval = value;
        		printf("Reporting interval set to %d\n", value);
        		message = "Reporting interval set";
        		command_success =  true;
        	}
        } else {
            printf("Unknown command \"%s\"\n", command);
            message = "Unknown command";
        }
    } else {
        printf("Failed to parse command. Command or argument missing?\n");
        message = "Parsing error";
    }

    // could be a command without ack, so ack ID can be null
    // the user needs to enable acknowledgments in the template to get an ack ID
    if (ack_id) {
        iotcl_mqtt_send_cmd_ack(
                ack_id,
                command_success ? IOTCL_C2D_EVT_CMD_SUCCESS_WITH_ACK : IOTCL_C2D_EVT_CMD_FAILED,
                message // allowed to be null, but should not be null if failed, we'd hope
        );
    } else {
        // if we send an ack
        printf("Message status is %s. Message: %s\n", command_success ? "SUCCESS" : "FAILED", message ? message : "<none>");
    }
}

static cy_rslt_t publish_telemetry(void) {
    ipc_payload_t payload = {0};
    cm33_ipc_safe_get_and_clear_cached_detection(&payload);
    IotclMessageHandle msg = iotcl_telemetry_create();
    iotcl_telemetry_set_string(msg, "version", APP_VERSION);
    iotcl_telemetry_set_number(msg, "random", rand() % 100); // test some random numbers
    iotcl_telemetry_set_number(msg, "class_id", payload.label_id);
    iotcl_telemetry_set_string(msg, "class", payload.label);
	iotcl_telemetry_set_bool(msg, "event_detected", payload.label_id > 0);

    iotcl_mqtt_send_telemetry(msg, false);
    iotcl_telemetry_destroy(msg);
    return CY_RSLT_SUCCESS;
}

static void user_input_yn_task (void *pvParameters) {
	TaskHandle_t *parent_task = pvParameters;

	user_input_status = APP_INPUT_NONE;
    // This happens probably due to UART getchar() spamming:
    printf("--- NOTE: You may notice temporary LCD corruption during this prompt ---\n");
    printf("Do you wish to configure the device?(y/[n]):\n>");

    int ch = getchar();
    if (EOF == ch) {
        printf("Got EOF?\n");
        goto done;
    }
    if (ch == 'y' || ch == 'Y') {
    	user_input_status = APP_INPUT_YES;
    } else {
    	user_input_status = APP_INPUT_NO;
    }
done:
	xTaskNotifyGive(*parent_task);
    while (1) {
		taskYIELD();
	}
}

void app_task(void *pvParameters) {
    (void) pvParameters;

    UBaseType_t my_priority = uxTaskPriorityGet(NULL);
    TaskHandle_t my_task = xTaskGetCurrentTaskHandle();

    // DO NOT print anything before we receive a message to avoice garbled output

    // we want to wait for CM33 to start receiving messages to prevent halts and errors below.
    while (!cm33_ipc_has_received_message()) {
        taskYIELD(); // wait for CM55
    }
    printf("\nApp Task: CM55 IPC is ready. Resuming the application...\n");

    char iotc_duid[IOTCL_CONFIG_DUID_MAX_LEN] = IOTCONNECT_DUID;
    if (0 == strlen(iotc_duid)) {
        uint64_t hwuid = Cy_SysLib_GetUniqueId();
        uint32_t hwuidhi = (uint32_t)(hwuid >> 32);
        // not using low bytes in the name because they appear to be the same across all boards of the same type
        // feel free to modify the application to use these bytes
        // uint32_t hwuidlo = (uint32_t)(hwuid & 0xFFFFFFFF);
        sprintf(iotc_duid, IOTCONNECT_DUID_PREFIX"%08lx", (unsigned long) hwuidhi);
        printf("Generated device unique ID (DUID) is: %s\n", iotc_duid);
    }

    if(app_eeprom_data_init()) {
        printf("Failed to initialize EEPROM. Cannot continue.\n");
        goto exit_cleanup;
    } else {
        printf("EEPROM data loaded successfully. Data is %s.\n", app_eeprom_data_is_valid() ? "valid" : "invalid");
    }

    IotConnectClientConfig config;
    iotconnect_sdk_init_config(&config);
    if (strlen(IOTCONNECT_DEVICE_CERT) > 0) {
        if (app_eeprom_data_is_valid()) {
            printf("Warning: Device certificate is set in app_config.h, but device configuration data exits in flash.\n");
            printf("Please either clear the certificate and private key form app_config.h\n");
            printf("or erase external flash using the ModusToolbox Programmer.\n");
            goto exit_cleanup;
        }
        printf("Using certificate from app_config.h\n");
    } else if (0 == strlen(app_eeprom_data_get_certificate(IOTCONNECT_DEVICE_CERT))) {
	    printf("\nThe board needs to be configured.\n");
	    app_eeprom_data_do_user_input(iotc_x509_generate_credentials);
    } else {
        // else ask the user if they want to re configure the board. Wait some time for user input...
    	TaskHandle_t user_input_yn_task_handle;
        xTaskCreate(user_input_yn_task, "User Input", 1024, &my_task, (my_priority - 1), &user_input_yn_task_handle);
        ulTaskNotifyTake(pdTRUE, 4000);
        vTaskDelete(user_input_yn_task_handle);

        switch (user_input_status) {
        	case  APP_INPUT_NONE:
        	    printf("Timed out waiting for user input. Resuming...\n");
        	    break;
        	case  APP_INPUT_YES:
        	    app_eeprom_data_do_user_input(iotc_x509_generate_credentials);
        	    break;
        	default:
        	    printf("Bypassing device configuration.\n");
        	    break;
        }
    }

    config.connection_type = app_eeprom_data_get_platform(IOTCONNECT_CONNECTION_TYPE);
    config.cpid = app_eeprom_data_get_cpid(IOTCONNECT_CPID);
    config.env =  app_eeprom_data_get_env(IOTCONNECT_ENV);
    config.duid = iotc_duid;
    config.qos = 1;
    config.verbose = true;
    config.x509_config.device_cert = app_eeprom_data_get_certificate(IOTCONNECT_DEVICE_CERT);
    config.x509_config.device_key = app_eeprom_data_get_private_key(IOTCONNECT_DEVICE_KEY);
    config.callbacks.status_cb = on_connection_status;
    config.callbacks.cmd_cb = on_command;
    config.callbacks.ota_cb = on_ota;

    const char * conn_type_str = "(UNKNOWN)";
    if (config.connection_type == IOTC_CT_AWS) {
        conn_type_str = "AWS";
    } else if(config.connection_type == IOTC_CT_AZURE) {
        conn_type_str = "Azure";
    }

    printf("Current Settings:\n");
    printf("Platform: %s\n", conn_type_str);
    printf("DUID: %s\n", config.duid);
    printf("CPID: %s\n", config.cpid);
    printf("ENV: %s\n", config.env);
    printf("WiFi SSID: %s\n", app_eeprom_data_get_wifi_ssid(WIFI_SSID));


    if (strlen(IOTCONNECT_DEVICE_CERT) > 0) {
        printf("Device certificate is set in app_config.h\n");
    } else if (strlen(app_eeprom_data_get_certificate(IOTCONNECT_DEVICE_CERT)) > 0) {
        printf("Device certificate is stored in EEPROM\n");
    }

    // This will not return if it fails
    wifi_app_connect(
        app_eeprom_data_get_wifi_ssid(WIFI_SSID),
        app_eeprom_data_get_wifi_pass(WIFI_PASSWORD)
    );

    iotc_mtb_time_obtain(IOTCONNECT_SNTP_SERVER);

    cy_rslt_t ret = iotconnect_sdk_init(&config);
    if (CY_RSLT_SUCCESS != ret) {
        printf("Failed to initialize the /IOTCONNECT SDK. Error code: %u\n", (unsigned int) ret);
        goto exit_cleanup;
    }

    // Smoke-test the AWS creds mTLS flow before connecting MQTT.
    int creds_status = iotconnect_sdk_obtain_aws_creds();

    if (0 != creds_status) {
        printf("AWS creds obtain failed (status=%d).\n", creds_status);
    } else {
        const IotclDraCredentialsResult *c = iotconnect_sdk_aws_creds_get();
        if (c) {
            printf("AWS creds:\n  AKID: %s\n  Secret: %s\n  Session: %s\n  Expires: %s (in %d s)\n",
                c->access_key_id, c->secret_access_key, c->session_token,
                c->expiration_str, iotconnect_sdk_aws_creds_seconds_until_expiry()
            );
        }
        app_webrtc_start();
    }
    
    for (int i = 0; i < 10; i++) {
        ret = iotconnect_sdk_connect();
        if (CY_RSLT_SUCCESS != ret) {
            printf("Failed to initialize the /IOTCONNECT SDK. Error code: %u\n", (unsigned int) ret);
            goto exit_cleanup;
        }
        
        int max_messages = is_demo_mode ? 6000 : 300;
        for (int j = 0; iotconnect_sdk_is_connected() && j < max_messages; j++) {
            cy_rslt_t result = publish_telemetry();
            if (result != CY_RSLT_SUCCESS) {
                break;
                }
            iotconnect_sdk_poll_inbound_mq(reporting_interval);
        }
        iotconnect_sdk_disconnect();
    }
    iotconnect_sdk_deinit();

    printf("\nAppTask Done.\n");
    while (1) {
        taskYIELD();
    }
    return;

    exit_cleanup:
    printf("\nError encountered. AppTask Done.\n");
    while (1) {
        taskYIELD();
    }
}
