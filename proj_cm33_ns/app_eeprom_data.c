/* SPDX-License-Identifier: MIT
 * Copyright (C) 2024 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com>, Shu Liu <shu.liu@avnet.com> et al.
 */

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

/* FreeRTOS header files */
#include "FreeRTOS.h"
#include "task.h"

#include "cybsp.h"
#include "cy_em_eeprom.h"
#include "iotcl_cfg.h"
#include "app_eeprom_data.h"

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#define LOGICAL_EEPROM_START             (0U)

/* EEPROM Configuration details. All the sizes mentioned are in bytes.
 * For details on how to configure these values refer to cy_em_eeprom.h. The
 * library documentation is provided in Em EEPROM API Reference Manual. The user
 * access it from ModusToolbox IDE Quick Panel > Documentation>
 * Cypress Em_EEPROM middleware API reference manual.
 */
#define EEPROM_SIZE                      (2048)
#define BLOCKING_WRITE                   (1U)
#define REDUNDANT_COPY                   (1U)
#define WEAR_LEVELLING_FACTOR            (2U)
#define SIMPLE_MODE                      (0U)
#define CY_EM_EEPROM_NVM_SIZEOF_ROW      (16U)

/* Set the macro NVM_REGION_TO_USE to either USER_NVM or
 * EMULATED_EEPROM_NVM to specify the region of the NVM used for
 * emulated EEPROM.
 */
#define USER_NVM                         (1U)
#define EMULATED_EEPROM_NVM              (0U)

#if CY_EM_EEPROM_SIZE
/* CY_EM_EEPROM_SIZE to determine whether the target has a dedicated EEPROM region or not */
#define NVM_REGION_TO_USE                EMULATED_EEPROM_NVM
#else
#define NVM_REGION_TO_USE                USER_NVM
#endif

#if defined(CY_EM_EEPROM_EEPROM_DATA_LEN)
#undef CY_EM_EEPROM_EEPROM_DATA_LEN
/* Defines the maximum data length that can be stored in one NVM row */
#define CY_EM_EEPROM_EEPROM_DATA_LEN(simple_mode) \
                (CY_EM_EEPROM_NVM_SIZEOF_ROW / (2UL - (simple_mode)))
#endif /* defined(CY_EM_EEPROM_EEPROM_DATA_LEN) */

#if defined(CY_EM_EEPROM_GET_NUM_ROWS_IN_EEPROM)
#undef CY_EM_EEPROM_GET_NUM_ROWS_IN_EEPROM
/* The number of NVM rows required to create an Em_EEPROM of data_size */
#define CY_EM_EEPROM_GET_NUM_ROWS_IN_EEPROM(data_size, simple_mode) \
                ((((data_size) - 1UL) / (CY_EM_EEPROM_EEPROM_DATA_LEN(simple_mode))) + 1UL)
#endif /* defined(CY_EM_EEPROM_GET_NUM_ROWS_IN_EEPROM) */

#if defined(CY_EM_EEPROM_GET_NUM_DATA)
#undef CY_EM_EEPROM_GET_NUM_DATA
/* Defines the size of NVM without wear leveling and redundant copy overhead */
#define CY_EM_EEPROM_GET_NUM_DATA(data_size, simple_mode) \
                (CY_EM_EEPROM_GET_NUM_ROWS_IN_EEPROM(data_size, simple_mode) * \
                CY_EM_EEPROM_NVM_SIZEOF_ROW)
#endif /* defined(CY_EM_EEPROM_GET_NUM_DATA) */

#if defined(CY_EM_EEPROM_GET_PHYSICAL_SIZE)
#undef CY_EM_EEPROM_GET_PHYSICAL_SIZE
/* Returns the size of NVM allocated for Em_EEPROM including wear leveling and a redundant copy overhead */
#define CY_EM_EEPROM_GET_PHYSICAL_SIZE(data_size, simple_mode, wear_leveling, redundant_copy) \
                (CY_EM_EEPROM_GET_NUM_DATA(data_size, simple_mode) * \
                ((((1UL - (simple_mode)) * (wear_leveling)) * ((redundant_copy) + 1UL)) + (simple_mode)))
#endif /* defined(CY_EM_EEPROM_GET_PHYSICAL_SIZE) */


#if ((USER_NVM) == (NVM_REGION_TO_USE)) /* In case of USER_NVM as Em_EEPROM use directly RRAM address */
#define CY_RRAM_ADDRESS                  (CYMEM_CM33_0_user_nvm_START) 
#endif


#if ((EMULATED_EEPROM_NVM) == (NVM_REGION_TO_USE))
CY_SECTION(".cy_em_eeprom") \
        CY_ALIGN(CY_EM_EEPROM_FLASH_SIZEOF_ROW) \
        uint8_t eeprom_storage[CY_EM_EEPROM_GET_PHYSICAL_SIZE(EEPROM_SIZE, SIMPLE_MODE, WEAR_LEVELLING_FACTOR, REDUNDANT_COPY)] = {0U};
#else
uint8_t *eeprom_storage = (uint8_t *)CY_RRAM_ADDRESS;
#endif /* #if(NVM_REGION_TO_USE) */

static cy_stc_eeprom_context_t eeprom_context;

static cy_stc_eeprom_config_t em_eeprom_config =
{
    .eepromSize         = EEPROM_SIZE,
    .simpleMode         = SIMPLE_MODE,
    .wearLevelingFactor = WEAR_LEVELLING_FACTOR,
    .redundantCopy      = REDUNDANT_COPY,
    .blockingWrite      = BLOCKING_WRITE,
};


#define APP_DATA_VERSION          0x32a9f200
#define WIFI_SSID_LEN					  32
#define WIFI_PASS_LEN					  64
#define PEM_PRVKEY_LEN					 256
#define PEM_CERT_LEN					1024


typedef struct AppEepromData {
    uint32_t version; // sanity check to determine whether the data is stored or not
    int32_t	platform;
    char cpid[IOTCL_CONFIG_CPID_MAX_LEN + 1];
    char env[IOTCL_CONFIG_ENV_MAX_LEN + 1];
    char wifi_ssid[WIFI_SSID_LEN + 1];
    char wifi_pass[WIFI_PASS_LEN + 1];
    char pem_private_key[PEM_PRVKEY_LEN + 1];
    char pem_certificate[PEM_CERT_LEN + 1];
} AppEepromData;

static AppEepromData app_eeprom_data = {0};


static void handle_error(uint32_t status, char *message)
{
    if (CY_EM_EEPROM_SUCCESS != status) {
        if (CY_EM_EEPROM_REDUNDANT_COPY_USED != status) {
            printf("%s Status=%lx", message, status);
        } else {
            printf("%s","Main copy is corrupted. Redundant copy in Emulated EEPROM is used \r\n");
        }
    }
}

static const char* get_str_value_or_default(const char * value, const char* default_value) {
	if (!value || 0 == strlen(value)) {
		return default_value;
	}
	return value;
}

IotConnectConnectionType app_eeprom_data_get_platform(IotConnectConnectionType default_value) {
	if (!app_eeprom_data_is_valid()) {
		return default_value;
	}
	return (IotConnectConnectionType) app_eeprom_data.platform;
}

bool app_eeprom_data_is_valid(void) {
	return app_eeprom_data.version == APP_DATA_VERSION;
}

const char* app_eeprom_data_get_cpid(const char* default_value) {
	if (!app_eeprom_data_is_valid()) {
		return default_value;
	}
	return get_str_value_or_default(app_eeprom_data.cpid, default_value);
}

const char* app_eeprom_data_get_env(const char* default_value) {
	if (!app_eeprom_data_is_valid()) {
		return default_value;
	}
	return get_str_value_or_default(app_eeprom_data.env, default_value);
}

const char* app_eeprom_data_get_wifi_ssid(const char* default_value) {
	if (!app_eeprom_data_is_valid()) {
		return default_value;
	}
	return get_str_value_or_default(app_eeprom_data.wifi_ssid, default_value);
}

const char* app_eeprom_data_get_wifi_pass(const char* default_value) {
	if (!app_eeprom_data_is_valid()) {
		return default_value;
	}
	return get_str_value_or_default(app_eeprom_data.wifi_pass, default_value);
}
const char* app_eeprom_data_get_certificate(const char* default_value) {
	if (!app_eeprom_data_is_valid()) {
		return default_value;
	}
	return get_str_value_or_default(app_eeprom_data.pem_certificate, default_value);
}
const char* app_eeprom_data_get_private_key(const char* default_value) {
	if (!app_eeprom_data_is_valid()) {
		return default_value;
	}
	return get_str_value_or_default(app_eeprom_data.pem_private_key, default_value);
}

void app_eeprom_data_do_user_input(IotcX509CredentialsGenerate x509_creds_generate_cb) {
	cy_en_em_eeprom_status_t eeprom_return_value;

	app_eeprom_data.cpid[0] = '\0';
	app_eeprom_data.env[0] = '\0';
	app_eeprom_data.wifi_ssid[0] = '\0';
	app_eeprom_data.wifi_pass[0] = '\0';

    if (!app_eeprom_data_is_valid()) {
    	// only overwrite cert/key once. If we have one, then keep it.
    	memset(app_eeprom_data.pem_certificate, 0, sizeof(app_eeprom_data.pem_certificate));
    	memset(app_eeprom_data.pem_private_key, 0, sizeof(app_eeprom_data.pem_private_key));
		int ret = x509_creds_generate_cb(
				app_eeprom_data.pem_certificate,
				sizeof(app_eeprom_data.pem_certificate),
				app_eeprom_data.pem_private_key,
				sizeof(app_eeprom_data.pem_private_key)
		);
		if (0 != ret) {
			printf("ERROR: Unable to generate the device certificate and key pair!\n");
			return;
		}
    }

	printf("\n===============================================================\n");
	printf("Please enter your device configuration\n");

	char platform_str[sizeof("aws")];

	do {
		printf("Platform (aws/az): \n>");
		fflush(stdout);
		scanf("%3s", platform_str);
		app_eeprom_data.platform = (int32_t) IOTC_CT_UNDEFINED;
		if (0 == strcmp("aws", platform_str)) {
			printf("Platform: aws\n");
			app_eeprom_data.platform = (int32_t) IOTC_CT_AWS;
		} else if(0 == strcmp("az", platform_str)) {
			printf("Platform: az\n");
			app_eeprom_data.platform = (int32_t) IOTC_CT_AZURE;
		}
	} while (app_eeprom_data.platform == (int32_t) IOTC_CT_UNDEFINED);

	printf("\n");
	do {
		printf("CPID: \n>");
		fflush(stdout);
		// use macro string conversion + concatenation with the #define max lengths
		scanf("%"STR(IOTCL_CONFIG_CPID_MAX_LEN)"s", app_eeprom_data.cpid);
		printf("%s\n", app_eeprom_data.cpid);
	} while (strlen(app_eeprom_data.cpid) == 0);

	printf("\n");
	do {
		printf("Environment: \n>");
		fflush(stdout);
		scanf("%"STR(IOTCL_CONFIG_ENV_MAX_LEN)"s", app_eeprom_data.env);
		printf("%s\n", app_eeprom_data.env);
	} while (strlen(app_eeprom_data.env) == 0);


	printf("\n");
	do {
		printf("WiFi SSID: \n>");
		fflush(stdout);
		scanf("%"STR(WIFI_SSID_LEN)"s", app_eeprom_data.wifi_ssid);
		printf("%s\n", app_eeprom_data.wifi_ssid);
	} while (strlen(app_eeprom_data.wifi_ssid) == 0);


	printf("\n");
	do {
		printf("WiFi Password: \n>");
		fflush(stdout);
		scanf("%"STR(WIFI_PASS_LEN)"s", app_eeprom_data.wifi_pass);
		printf("%s\n\n", app_eeprom_data.wifi_pass);
	} while (strlen(app_eeprom_data.wifi_pass) == 0);

    // make data valid before writing
    app_eeprom_data.version = APP_DATA_VERSION;

	printf("\nWriting values...\n");
	vTaskDelay(pdMS_TO_TICKS(200));

	eeprom_return_value = Cy_Em_EEPROM_Write(0, &app_eeprom_data, sizeof(app_eeprom_data), &eeprom_context);
	handle_error(eeprom_return_value, "Emulated EEPROM Write failed \r\n");

	if (CY_EM_EEPROM_SUCCESS == eeprom_return_value) {
		printf("Resetting the board...\n");
	}

	vTaskDelay(pdMS_TO_TICKS(200));
	NVIC_SystemReset();
}

void app_eeprom_save_dummy_data() {
    app_eeprom_data.version = APP_DATA_VERSION;
    app_eeprom_data.platform = (int32_t) IOTC_CT_AWS;
    strncpy(app_eeprom_data.cpid, "my_cpid", sizeof(app_eeprom_data.cpid));
    strncpy(app_eeprom_data.env, "my_env", sizeof(app_eeprom_data.env));
    strncpy(app_eeprom_data.wifi_ssid, "my_wifi_ssid", sizeof(app_eeprom_data.wifi_ssid));
    strncpy(app_eeprom_data.wifi_pass, "my_wifi_pass", sizeof(app_eeprom_data.wifi_pass));
    strncpy(app_eeprom_data.pem_certificate, "my_cert", sizeof(app_eeprom_data.pem_certificate));
    strncpy(app_eeprom_data.pem_private_key, "my_key", sizeof(app_eeprom_data.pem_private_key));
    cy_en_em_eeprom_status_t eeprom_return_value;
    eeprom_return_value = Cy_Em_EEPROM_Write(LOGICAL_EEPROM_START, &app_eeprom_data, sizeof(app_eeprom_data), &eeprom_context);
	handle_error(eeprom_return_value, "Emulated EEPROM Write failed \r\n");
}

int app_eeprom_data_init(void) {
    cy_en_em_eeprom_status_t eeprom_return_value;

	/* Initialize the flash start address in EEPROM configuration structure. */
    em_eeprom_config.userFlashStartAddr = (uint32_t)eeprom_storage;

	eeprom_return_value = Cy_Em_EEPROM_Init(&em_eeprom_config, &eeprom_context);
	handle_error(eeprom_return_value, "Emulated EEPROM Initialization Error \r\n");

	/* Read 81 bytes out of EEPROM memory. */
	eeprom_return_value = Cy_Em_EEPROM_Read(LOGICAL_EEPROM_START, &app_eeprom_data, sizeof(app_eeprom_data), &eeprom_context);
	handle_error(eeprom_return_value, "Emulated EEPROM Read failed \r\n");

	return eeprom_return_value;
}
