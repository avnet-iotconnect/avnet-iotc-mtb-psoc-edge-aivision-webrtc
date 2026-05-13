/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "iotconnect.h"
#include "iotcl.h"

#include "webrtc/app_webrtc.h"

#include "aws_creds.h"
#include "csprng.h"
#include "ice_controller.h"
#include "peer_connection.h"
#include "peer_connection_sdp.h"
#include "signaling.h"
#include "transceiver_data_types.h"

#define APP_WEBRTC_WSS_ENDPOINT_LEN 256U
#define APP_WEBRTC_AWS_REGION_MAXLEN 50U
#define APP_WEBRTC_SDP_BUF_LEN (12U * 1024U)

#define APP_WEBRTC_TRANSCEIVER_STREAM_ID "myKvsVideoStream"
#define APP_WEBRTC_TRANSCEIVER_VIDEO_TRACK_ID "myVideoTrack"
#define APP_WEBRTC_TRANSCEIVER_ROLLING_BUFFER_SEC 3U
#define APP_WEBRTC_TRANSCEIVER_H264_BITRATE_BPS (1400U * 1024U)

#define WEBRTC_TASK_NAME "webrtc"
#define WEBRTC_TASK_STACK_W 2048U
#define WEBRTC_TASK_PRIO (tskIDLE_PRIORITY + 2)

#define APP_WEBRTC_POLL_IDLE_MS 20U
#define APP_WEBRTC_BACKOFF_MS 1000U
#define APP_WEBRTC_STOP_TIMEOUT_MS 5000U

static TaskHandle_t s_webrtc_task = NULL;
static volatile bool s_start_requested = false;
static volatile bool s_creds_dirty = false;
static bool s_csprng_ready = false;
static char s_wss_endpoint[APP_WEBRTC_WSS_ENDPOINT_LEN];
static SignalingHandle s_active_signaling = NULL;


// Pull the cached IoTC-discovered AWS triplet and channel ARN into our local
// view. The strings stay owned by the SDK cache.
static int populate_creds(AwsCreds *out, char *region_buf, size_t region_buf_size) {
    const IotclDraCredentialsResult *creds = iotconnect_sdk_aws_creds_get();
    if (NULL == creds) {
        printf("[webrtc] no AWS creds cached (or expired)\n");
        return -1;
    }

    IotclMqttConfig *mqtt_cfg = iotcl_mqtt_get_config();
    if (NULL == mqtt_cfg || NULL == mqtt_cfg->aws.webrtc_channel_arn) {
        printf("[webrtc] webrtc_channel_arn not available from /IOTCONNECT discovery\n");
        return -1;
    }

    const char *arn = mqtt_cfg->aws.webrtc_channel_arn;
    int colon_count = 0;
    const char *region_start = NULL;
    region_buf[0] = '\0';
    for (const char *p = arn; '\0' != *p; p++) {
        if (':' != *p) {
            continue;
        }

        colon_count++;
        if (3 == colon_count) {
            region_start = p + 1;
            continue;
        }

        if (4 == colon_count && NULL != region_start) {
            size_t region_len = (size_t) (p - region_start);
            if (0U == region_len || region_len >= region_buf_size) {
                printf("[webrtc] region in ARN is invalid or too long\n");
                return -1;
            }
            memcpy(region_buf, region_start, region_len);
            region_buf[region_len] = '\0';
            break;
        }
    }

    if (NULL == region_start || '\0' == region_buf[0]) {
        printf("[webrtc] failed to parse region from ARN: %s\n", arn);
        return -1;
    }

    out->region = region_buf;
    out->channel_arn = mqtt_cfg->aws.webrtc_channel_arn;
    out->client_id = mqtt_cfg->client_id;
    out->access_key_id = creds->access_key_id;
    out->secret_access_key = creds->secret_access_key;
    out->session_token = creds->session_token;
    return 0;
}


static int ensure_csprng_ready(void) {
    if (s_csprng_ready) {
        return 0;
    }

    if (0 != webrtc_csprng_init()) {
        printf("[webrtc] csprng init failed\n");
        return -1;
    }

    s_csprng_ready = true;
    return 0;
}


static void init_video_transceiver(Transceiver_t *out_transceiver) {
    memset(out_transceiver, 0, sizeof(*out_transceiver));
    out_transceiver->trackKind = TRANSCEIVER_TRACK_KIND_VIDEO;
    out_transceiver->direction = TRANSCEIVER_TRACK_DIRECTION_SENDONLY;
    TRANSCEIVER_ENABLE_CODEC(
        out_transceiver->codecBitMap,
        TRANSCEIVER_RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_BIT
    );
    out_transceiver->rollingbufferDurationSec = APP_WEBRTC_TRANSCEIVER_ROLLING_BUFFER_SEC;
    out_transceiver->rollingbufferBitRate = APP_WEBRTC_TRANSCEIVER_H264_BITRATE_BPS;
    memcpy(
        out_transceiver->streamId,
        APP_WEBRTC_TRANSCEIVER_STREAM_ID,
        sizeof(APP_WEBRTC_TRANSCEIVER_STREAM_ID)
    );
    out_transceiver->streamIdLength = sizeof(APP_WEBRTC_TRANSCEIVER_STREAM_ID) - 1U;
    memcpy(
        out_transceiver->trackId,
        APP_WEBRTC_TRANSCEIVER_VIDEO_TRACK_ID,
        sizeof(APP_WEBRTC_TRANSCEIVER_VIDEO_TRACK_ID)
    );
    out_transceiver->trackIdLength = sizeof(APP_WEBRTC_TRANSCEIVER_VIDEO_TRACK_ID) - 1U;
}


static int build_answer_from_offer(
    PeerConnectionSession_t *session,
    char *offer_sdp,
    size_t offer_len,
    char *local_desc_buffer,
    size_t local_desc_buffer_len,
    char *answer_buffer,
    size_t answer_buffer_len,
    size_t *out_answer_len
) {
    PeerConnectionBufferSessionDescription_t remote_desc = {
        .pSdpBuffer = offer_sdp,
        .sdpBufferLength = offer_len,
        .type = SDP_CONTROLLER_MESSAGE_TYPE_OFFER,
    };
    PeerConnectionBufferSessionDescription_t local_desc = {
        .pSdpBuffer = local_desc_buffer,
        .sdpBufferLength = local_desc_buffer_len,
        .type = SDP_CONTROLLER_MESSAGE_TYPE_ANSWER,
    };
    PeerConnectionResult_t pc_rc;

    *out_answer_len = answer_buffer_len;

    pc_rc = PeerConnectionSdp_DeserializeSdpMessage(&remote_desc);
    if (PEER_CONNECTION_RESULT_OK != pc_rc) {
        printf("[webrtc] PeerConnectionSdp_DeserializeSdpMessage failed: %d\n", (int) pc_rc);
        return -1;
    }

    pc_rc = PeerConnectionSdp_SetPayloadTypes(session, &remote_desc);
    if (PEER_CONNECTION_RESULT_OK != pc_rc) {
        printf("[webrtc] PeerConnectionSdp_SetPayloadTypes failed: %d\n", (int) pc_rc);
        return -1;
    }

    // fork-aivision: PopulateSessionDescription reads TWCC info back from the
    // session's cached remoteSessionDescription on the answer path, so keep the
    // parsed offer struct there for this S6b offer->answer round-trip.
    session->remoteSessionDescription = remote_desc;

    pc_rc = PeerConnectionSdp_PopulateSessionDescription(
        session,
        &remote_desc,
        &local_desc,
        answer_buffer,
        out_answer_len
    );
    if (PEER_CONNECTION_RESULT_OK != pc_rc) {
        printf("[webrtc] PeerConnectionSdp_PopulateSessionDescription failed: %d\n", (int) pc_rc);
        return -1;
    }

    pc_rc = PeerConnection_SetLocalDescription(session, &local_desc);
    if (PEER_CONNECTION_RESULT_OK != pc_rc) {
        printf("[webrtc] PeerConnection_SetLocalDescription failed: %d\n", (int) pc_rc);
        return -1;
    }

    return 0;
}


static int run_session(void) {
    char region_buf[APP_WEBRTC_AWS_REGION_MAXLEN + 1U];
    AwsCreds aws_creds;
    char *signed_url = NULL;
    char *offer_sdp = NULL;
    char *local_desc_buffer = NULL;
    char *answer_buffer = NULL;
    SignalingHandle sig = NULL;
    PeerConnectionSession_t session;
    PeerConnectionSessionConfiguration_t pc_config;
    Transceiver_t video_transceiver;
    bool peer_connection_inited = false;
    size_t offer_len = 0;
    int rc = -1;

    if (0 != ensure_csprng_ready()) {
        return -1;
    }

    if (0 != populate_creds(&aws_creds, region_buf, sizeof(region_buf))) {
        return -1;
    }

    size_t signed_url_cap = strlen(s_wss_endpoint)
        + (strlen(aws_creds.channel_arn) * 3U)
        + (strlen(aws_creds.session_token) * 3U)
        + 512U;

    signed_url = malloc(signed_url_cap);
    offer_sdp = malloc(APP_WEBRTC_SDP_BUF_LEN);
    local_desc_buffer = malloc(PEER_CONNECTION_SDP_DESCRIPTION_BUFFER_MAX_LENGTH);
    answer_buffer = malloc(PEER_CONNECTION_SDP_DESCRIPTION_BUFFER_MAX_LENGTH + 1U);
    if (NULL == signed_url || NULL == offer_sdp || NULL == local_desc_buffer || NULL == answer_buffer) {
        printf("[webrtc] OOM allocating session buffers\n");
        goto cleanup;
    }

    if (0 != signaling_build_signed_url(&aws_creds, s_wss_endpoint, signed_url, signed_url_cap)) {
        printf("[webrtc] signaling_build_signed_url failed\n");
        goto cleanup;
    }

    sig = signaling_connect(signed_url);
    if (NULL == sig) {
        printf("[webrtc] signaling_connect failed\n");
        goto cleanup;
    }

    s_active_signaling = sig;

    printf("[webrtc] waiting for SDP offer...\n");
    if (0 != signaling_wait_for_offer(sig, offer_sdp, APP_WEBRTC_SDP_BUF_LEN, &offer_len)) {
        printf("[webrtc] signaling_wait_for_offer failed\n");
        goto cleanup;
    }

    memset(&pc_config, 0, sizeof(pc_config));
    pc_config.canTrickleIce = 1U;
    pc_config.natTraversalConfigBitmap =
        ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_SEND_HOST |
        ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_ACCEPT_HOST;

    if (PEER_CONNECTION_RESULT_OK != PeerConnection_Init(&session, &pc_config)) {
        printf("[webrtc] PeerConnection_Init failed\n");
        goto cleanup;
    }
    peer_connection_inited = true;

    init_video_transceiver(&video_transceiver);
    if (PEER_CONNECTION_RESULT_OK != PeerConnection_AddTransceiver(&session, &video_transceiver)) {
        printf("[webrtc] PeerConnection_AddTransceiver failed\n");
        goto cleanup;
    }

    if (PEER_CONNECTION_RESULT_OK != PeerConnection_Start(&session)) {
        printf("[webrtc] PeerConnection_Start failed\n");
        goto cleanup;
    }

    // fork-aivision: S6b stops at offer->answer. Keep signaling attached only
    // to the WSS socket for now; the peer-session/trickle-ICE bridge lands
    // with the later lifecycle + ICE adaptation work.
    size_t answer_len = PEER_CONNECTION_SDP_DESCRIPTION_BUFFER_MAX_LENGTH;
    if (0 != build_answer_from_offer(
        &session,
        offer_sdp,
        offer_len,
        local_desc_buffer,
        PEER_CONNECTION_SDP_DESCRIPTION_BUFFER_MAX_LENGTH,
        answer_buffer,
        PEER_CONNECTION_SDP_DESCRIPTION_BUFFER_MAX_LENGTH,
        &answer_len
    )) {
        goto cleanup;
    }

    answer_buffer[answer_len] = '\0';
    if (0 != signaling_send_answer(sig, answer_buffer)) {
        printf("[webrtc] signaling_send_answer failed\n");
        goto cleanup;
    }

    printf("[webrtc] answer queued (%d bytes), entering signaling tick loop\n", (int) answer_len);
    while (s_start_requested) {
        if (0 != signaling_tick(sig)) {
            printf("[webrtc] signaling_tick reported disconnect\n");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_POLL_IDLE_MS));
    }

    rc = 0;

cleanup:
    if (peer_connection_inited) {
        (void) PeerConnection_CloseSession(&session);
    }
    if (NULL != sig) {
        signaling_disconnect(sig);
    }
    if (s_active_signaling == sig) {
        s_active_signaling = NULL;
    }
    free(answer_buffer);
    free(local_desc_buffer);
    free(offer_sdp);
    free(signed_url);
    return rc;
}


static void webrtc_task(void *arg) {
    (void) arg;

    for (;;) {
        if (!s_start_requested) {
            vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_POLL_IDLE_MS));
            continue;
        }

        if (s_creds_dirty) {
            s_creds_dirty = false;
            printf("[webrtc] creds_dirty observed at session boundary (no-op in S6b)\n");
        }

        int rc = run_session();
        if (!s_start_requested) {
            continue;
        }

        printf("[webrtc] session ended rc=%d, retrying in %d ms\n", rc, (int) APP_WEBRTC_BACKOFF_MS);
        vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_BACKOFF_MS));
    }
}


void app_webrtc_init(void) {
    if (NULL != s_webrtc_task) {
        return;
    }

    BaseType_t rc = xTaskCreate(
        webrtc_task,
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
    char region_buf[APP_WEBRTC_AWS_REGION_MAXLEN + 1U];
    AwsCreds aws_creds;

    if (NULL == s_webrtc_task) {
        printf("[webrtc] start before init\n");
        return false;
    }
    if (s_start_requested) {
        return true;
    }

    if (0 != populate_creds(&aws_creds, region_buf, sizeof(region_buf))) {
        return false;
    }
    if (0 != signaling_resolve_endpoint(&aws_creds, s_wss_endpoint, sizeof(s_wss_endpoint))) {
        printf("[webrtc] signaling_resolve_endpoint failed\n");
        return false;
    }

    s_start_requested = true;
    printf("[webrtc] session loop enabled\n");
    return true;
}


void app_webrtc_stop(void) {
    s_start_requested = false;

    if (NULL != s_active_signaling) {
        signaling_disconnect(s_active_signaling);
        s_active_signaling = NULL;
    }

    (void) APP_WEBRTC_STOP_TIMEOUT_MS;
    vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_POLL_IDLE_MS * 2U));
    printf("[webrtc] stopped\n");
}


void app_webrtc_notify_creds_updated(void) {
    s_creds_dirty = true;
}
