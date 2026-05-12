/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 *
 * CSPRNG/DRBG for the WebRTC subsystem. See csprng.h for rationale.
 *
 * Seeded from mbedtls_entropy_func, which on this build pulls from
 * mbedtls_hardware_poll (the Infineon TRNG port) because
 * MBEDTLS_ENTROPY_HARDWARE_ALT + MBEDTLS_NO_PLATFORM_ENTROPY are set in
 * mbedtls_user_config.h.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

#include "csprng.h"

// Personalization string for the DRBG. Mixed into the seed alongside the
// hardware entropy. Project-stable so reseed across reboots stays distinct
// from the TLS/MQTT DRBG's defaults; not security-critical on its own.
static const unsigned char DRBG_PERSONALIZATION[] = "fork-aivision/webrtc/v1";

static mbedtls_entropy_context entropy_ctx;
static mbedtls_ctr_drbg_context drbg_ctx;
static SemaphoreHandle_t drbg_mutex = NULL;
static bool drbg_ready = false;


int webrtc_csprng_init(void) {
    if (drbg_ready) {
        return 0;
    }

    if (NULL == drbg_mutex) {
        drbg_mutex = xSemaphoreCreateMutex();
        if (NULL == drbg_mutex) {
            printf("[csprng] mutex alloc failed\n");
            return -1;
        }
    }

    mbedtls_entropy_init(&entropy_ctx);
    mbedtls_ctr_drbg_init(&drbg_ctx);

    int rc = mbedtls_ctr_drbg_seed(
        &drbg_ctx,
        mbedtls_entropy_func, &entropy_ctx,
        DRBG_PERSONALIZATION, sizeof(DRBG_PERSONALIZATION) - 1
    );
    if (0 != rc) {
        printf("[csprng] mbedtls_ctr_drbg_seed failed: -0x%04x\n", (unsigned) -rc);
        mbedtls_ctr_drbg_free(&drbg_ctx);
        mbedtls_entropy_free(&entropy_ctx);
        return -1;
    }

    // Prediction resistance off — costly and not required for our use cases
    // (handshake nonces, ICE creds, cert serial). Reseed interval is the
    // mbedTLS default (10000 requests).
    mbedtls_ctr_drbg_set_prediction_resistance(&drbg_ctx, MBEDTLS_CTR_DRBG_PR_OFF);

    drbg_ready = true;
    return 0;
}


int webrtc_csprng_bytes(uint8_t *out_buf, size_t len) {
    if (NULL == out_buf || 0 == len) {
        return -1;
    }
    if (!drbg_ready) {
        printf("[csprng] bytes requested before init\n");
        return -1;
    }

    if (pdTRUE != xSemaphoreTake(drbg_mutex, portMAX_DELAY)) {
        return -1;
    }
    int rc = mbedtls_ctr_drbg_random(&drbg_ctx, out_buf, len);
    xSemaphoreGive(drbg_mutex);

    if (0 != rc) {
        printf("[csprng] mbedtls_ctr_drbg_random failed: -0x%04x\n", (unsigned) -rc);
        return -1;
    }
    return 0;
}


int webrtc_csprng_f_rng(void *p_rng, unsigned char *out, size_t len) {
    (void) p_rng;
    return webrtc_csprng_bytes(out, len);
}


void *webrtc_csprng_p_rng(void) {
    // Returned for API symmetry; webrtc_csprng_f_rng ignores it and grabs the
    // internal mutex itself. Future callers that bypass the wrapper and call
    // mbedtls_ctr_drbg_random directly would race — don't.
    return &drbg_ctx;
}
