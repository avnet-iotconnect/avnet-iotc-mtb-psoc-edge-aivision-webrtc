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
#include "demo_config.h"
#include "ice_data_types.h"
#include "ice_controller.h"
#include "peer_connection.h"
#include "signaling.h"
#include "srtp.h"
#include "stun_data_types.h"
#include "transceiver_data_types.h"

#define APP_WEBRTC_WSS_ENDPOINT_LEN 256U
#define APP_WEBRTC_AWS_REGION_MAXLEN 50U
#define APP_WEBRTC_SDP_BUF_LEN (12U * 1024U)

#define APP_WEBRTC_TRANSCEIVER_STREAM_ID "myKvsVideoStream"
#define APP_WEBRTC_TRANSCEIVER_VIDEO_TRACK_ID "myVideoTrack"
#define APP_WEBRTC_TRANSCEIVER_VIDEO_MID "0"
#define APP_WEBRTC_TRANSCEIVER_AUDIO_TRACK_ID "myAudioTrack"
#define APP_WEBRTC_TRANSCEIVER_ROLLING_BUFFER_SEC 3U
#define APP_WEBRTC_TRANSCEIVER_H264_BITRATE_BPS (1400U * 1024U)
#define APP_WEBRTC_TRANSCEIVER_OPUS_BITRATE_BPS  (64U * 1024U)

#define WEBRTC_TASK_NAME "webrtc"
#define WEBRTC_TASK_STACK_W (110U * 1024U)
#define WEBRTC_TASK_PRIO (tskIDLE_PRIORITY + 2)

#define APP_WEBRTC_POLL_IDLE_MS 20U
#define APP_WEBRTC_BACKOFF_MS 1000U
#define APP_WEBRTC_LOCAL_CANDIDATE_BUF_LEN 192U

static TaskHandle_t s_webrtc_task = NULL;
static volatile bool s_streaming_requested = false;
static volatile bool s_creds_dirty = false;

typedef struct AppWebrtcSignalingBridge {
    SignalingHandle sig;
    const char *sdp_mid;
    int sdp_m_line_index;
} AppWebrtcSignalingBridge_t;

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

static int signaling_open_session(
    const AwsCreds *creds,
    const char *wss_endpoint,
    SignalingHandle *out_sig
) {
    size_t signed_url_cap = strlen(wss_endpoint)
        + (strlen(creds->channel_arn) * 3U)
        + (strlen(creds->session_token) * 3U)
        + 512U;

    char *signed_url = malloc(signed_url_cap);
    if (NULL == signed_url) {
        printf("[webrtc] OOM allocating signed URL (%u bytes)\n", (unsigned) signed_url_cap);
        return -1;
    }

    SignalingHandle sig = NULL;
    if (0 == signaling_build_signed_url(creds, wss_endpoint, signed_url, signed_url_cap)) {
        sig = signaling_connect(signed_url);
        if (NULL == sig) {
            printf("[webrtc] signaling_connect failed\n");
        }
    } else {
        printf("[webrtc] signaling_build_signed_url failed\n");
    }
    free(signed_url);

    if (NULL == sig) {
        return -1;
    }
    *out_sig = sig;
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

/* Phantom audio transceiver: browsers that offer video+audio (Chrome default)
 * need an m=audio in the answer or BUNDLE goes mismatched and Chrome silently
 * rejects. Upstream's PopulateMediaDescriptions only emits answer m-sections
 * for matched transceivers, so we add this Opus placeholder that never streams. */
static void init_audio_transceiver(Transceiver_t *out_transceiver) {
    memset(out_transceiver, 0, sizeof(*out_transceiver));
    out_transceiver->trackKind = TRANSCEIVER_TRACK_KIND_AUDIO;
    out_transceiver->direction = TRANSCEIVER_TRACK_DIRECTION_SENDRECV;
    TRANSCEIVER_ENABLE_CODEC(out_transceiver->codecBitMap, TRANSCEIVER_RTC_CODEC_OPUS_BIT);
    out_transceiver->rollingbufferDurationSec = APP_WEBRTC_TRANSCEIVER_ROLLING_BUFFER_SEC;
    out_transceiver->rollingbufferBitRate = APP_WEBRTC_TRANSCEIVER_OPUS_BITRATE_BPS;
    memcpy(
        out_transceiver->streamId,
        APP_WEBRTC_TRANSCEIVER_STREAM_ID,
        sizeof(APP_WEBRTC_TRANSCEIVER_STREAM_ID)
    );
    out_transceiver->streamIdLength = sizeof(APP_WEBRTC_TRANSCEIVER_STREAM_ID) - 1U;
    memcpy(
        out_transceiver->trackId,
        APP_WEBRTC_TRANSCEIVER_AUDIO_TRACK_ID,
        sizeof(APP_WEBRTC_TRANSCEIVER_AUDIO_TRACK_ID)
    );
    out_transceiver->trackIdLength = sizeof(APP_WEBRTC_TRANSCEIVER_AUDIO_TRACK_ID) - 1U;
}

static const char *local_candidate_type_string(IceCandidateType_t candidate_type) {
    switch (candidate_type) {
        case ICE_CANDIDATE_TYPE_HOST:
            return "host";
        case ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
            return "srflx";
        case ICE_CANDIDATE_TYPE_PEER_REFLEXIVE:
            return "prflx";
        case ICE_CANDIDATE_TYPE_RELAY:
            return "relay";
        default:
            return "unknown";
    }
}

static int format_local_candidate_ip(const IceCandidate_t *candidate, char *ip_buf, size_t ip_buf_len) {
    if (NULL == candidate || NULL == ip_buf || ip_buf_len < 16U) {
        return -1;
    }

    if (STUN_ADDRESS_IPv4 != candidate->endpoint.transportAddress.family) {
        printf("[webrtc] local ICE candidate family %d not supported for signaling\n",
               (int) candidate->endpoint.transportAddress.family);
        return -1;
    }

    const uint8_t *ip = candidate->endpoint.transportAddress.address;
    int ip_len = snprintf(ip_buf, ip_buf_len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    if (ip_len <= 0 || (size_t) ip_len >= ip_buf_len) {
        return -1;
    }

    return 0;
}

static int serialize_local_candidate(
    const PeerConnectionIceLocalCandidate_t *local_candidate,
    char *candidate_buf,
    size_t candidate_buf_len
) {
    char ip_buf[16];
    const IceCandidate_t *candidate;
    const char *candidate_type;

    if (NULL == local_candidate || NULL == candidate_buf || candidate_buf_len < 2U) {
        return -1;
    }

    candidate = local_candidate->pLocalCandidate;
    if (NULL == candidate) {
        return -1;
    }

    if (0 != format_local_candidate_ip(candidate, ip_buf, sizeof(ip_buf))) {
        return -1;
    }

    candidate_type = local_candidate_type_string(candidate->candidateType);

    int candidate_len = snprintf(
        candidate_buf,
        candidate_buf_len,
        "candidate:%u 1 udp %lu %s %u typ %s",
        (unsigned int) local_candidate->localCandidateIndex,
        (unsigned long) candidate->priority,
        ip_buf,
        (unsigned int) candidate->endpoint.transportAddress.port,
        candidate_type
    );
    if (candidate_len <= 0 || (size_t) candidate_len >= candidate_buf_len) {
        return -1;
    }

    return 0;
}

static void on_local_candidate_ready(void *context, PeerConnectionIceLocalCandidate_t *local_candidate) {
    AppWebrtcSignalingBridge_t *bridge = (AppWebrtcSignalingBridge_t *) context;
    char candidate_buf[APP_WEBRTC_LOCAL_CANDIDATE_BUF_LEN];

    if (NULL == bridge || NULL == bridge->sig || NULL == local_candidate) {
        return;
    }

    if (0 != serialize_local_candidate(local_candidate, candidate_buf, sizeof(candidate_buf))) {
        printf("[webrtc] failed to serialize local ICE candidate\n");
        return;
    }

    printf("[webrtc] local ICE candidate ready: %s\n", candidate_buf);
    if (0 != signaling_send_ice_candidate(
        bridge->sig,
        candidate_buf,
        bridge->sdp_mid,
        bridge->sdp_m_line_index
    )) {
        printf("[webrtc] signaling_send_ice_candidate failed\n");
    }
}

static int run_session(void) {
    char region_buf[APP_WEBRTC_AWS_REGION_MAXLEN + 1U];
    char wss_endpoint[APP_WEBRTC_WSS_ENDPOINT_LEN];
    AwsCreds aws_creds;
    PeerConnectionSession_t session = {0};
    /* Transceivers must outlive the PeerConnection they're added to:
     * PeerConnection_AddTransceiver stores raw pointers, not copies, and
     * SetPayloadType later dereferences ->codecBitMap and ->trackKind. */
    Transceiver_t video_transceiver;
    Transceiver_t audio_transceiver;
    SignalingHandle sig = NULL;
    AppWebrtcSignalingBridge_t signaling_bridge = {
        .sig = NULL,
        .sdp_mid = APP_WEBRTC_TRANSCEIVER_VIDEO_MID,
        .sdp_m_line_index = 0,
    };
    bool peer_connection_inited = false;
    int rc = -1;

    if (0 != populate_creds(&aws_creds, region_buf, sizeof(region_buf))) {
        return -1;
    }
    if (0 != signaling_resolve_endpoint(&aws_creds, wss_endpoint, sizeof(wss_endpoint))) {
        printf("[webrtc] signaling_resolve_endpoint failed\n");
        return -1;
    }

    if (0 != signaling_open_session(&aws_creds, wss_endpoint, &sig)) {
        goto cleanup;
    }

    {
        PeerConnectionSessionConfiguration_t pc_config;
        memset(&pc_config, 0, sizeof(pc_config));
        pc_config.canTrickleIce = 1U;
        pc_config.natTraversalConfigBitmap =
            ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_SEND_HOST |
            ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_ACCEPT_HOST |
            ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_ACCEPT_SRFLX;
        #if APP_WEBRTC_ENABLE_SRFLX
        pc_config.natTraversalConfigBitmap |= ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_SEND_SRFLX;
        #endif
        #if APP_WEBRTC_ENABLE_TURN
        pc_config.natTraversalConfigBitmap |=
            ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_SEND_RELAY |
            ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_ACCEPT_RELAY;
        #endif

        if (PEER_CONNECTION_RESULT_OK != PeerConnection_Init(&session, &pc_config)) {
            printf("[webrtc] PeerConnection_Init failed\n");
            goto cleanup;
        }
        peer_connection_inited = true;

        init_video_transceiver(&video_transceiver);
        if (PEER_CONNECTION_RESULT_OK != PeerConnection_AddTransceiver(&session, &video_transceiver)) {
            printf("[webrtc] PeerConnection_AddTransceiver(video) failed\n");
            goto cleanup;
        }
        init_audio_transceiver(&audio_transceiver);
        if (PEER_CONNECTION_RESULT_OK != PeerConnection_AddTransceiver(&session, &audio_transceiver)) {
            printf("[webrtc] PeerConnection_AddTransceiver(audio) failed\n");
            goto cleanup;
        }
        if (PEER_CONNECTION_RESULT_OK != PeerConnection_SetOnLocalCandidateReady(&session, on_local_candidate_ready, &signaling_bridge)) {
            printf("[webrtc] PeerConnection_SetOnLocalCandidateReady failed\n");
            goto cleanup;
        }
        if (PEER_CONNECTION_RESULT_OK != PeerConnection_Start(&session)) {
            printf("[webrtc] PeerConnection_Start failed\n");
            goto cleanup;
        }
    }
    signaling_bridge.sig = sig;
    signaling_set_peer_connection(sig, &session);

    {
        /* SetRemoteDescription copies the offer into session->remoteSdpBuffer,
         * so offer_sdp can drop out of scope after the call returns. */
        char offer_sdp[APP_WEBRTC_SDP_BUF_LEN];
        size_t offer_len = 0;
        PeerConnectionBufferSessionDescription_t remote_desc;

        printf("[webrtc] waiting for SDP offer...\n");
        if (0 != signaling_wait_for_offer(sig, offer_sdp, sizeof(offer_sdp), &offer_len)) {
            printf("[webrtc] signaling_wait_for_offer failed\n");
            goto cleanup;
        }

        memset(&remote_desc, 0, sizeof(remote_desc));
        remote_desc.pSdpBuffer = offer_sdp;
        remote_desc.sdpBufferLength = offer_len;
        remote_desc.type = SDP_CONTROLLER_MESSAGE_TYPE_OFFER;

        if (PEER_CONNECTION_RESULT_OK != PeerConnection_SetRemoteDescription(&session, &remote_desc)) {
            printf("[webrtc] PeerConnection_SetRemoteDescription failed\n");
            goto cleanup;
        }
    }

    {
        char local_desc_buffer[PEER_CONNECTION_SDP_DESCRIPTION_BUFFER_MAX_LENGTH];
        char answer_buffer[PEER_CONNECTION_SDP_DESCRIPTION_BUFFER_MAX_LENGTH + 1U];
        PeerConnectionBufferSessionDescription_t answer_desc;
        size_t answer_len = PEER_CONNECTION_SDP_DESCRIPTION_BUFFER_MAX_LENGTH;

        memset(&answer_desc, 0, sizeof(answer_desc));
        answer_desc.pSdpBuffer = local_desc_buffer;
        answer_desc.sdpBufferLength = sizeof(local_desc_buffer);
        answer_desc.type = SDP_CONTROLLER_MESSAGE_TYPE_ANSWER;

        if (PEER_CONNECTION_RESULT_OK != PeerConnection_CreateAnswer(&session, &answer_desc, answer_buffer, &answer_len)) {
            printf("[webrtc] PeerConnection_CreateAnswer failed\n");
            goto cleanup;
        }

        if (PEER_CONNECTION_RESULT_OK != PeerConnection_SetLocalDescription(&session, &answer_desc)) {
            printf("[webrtc] PeerConnection_SetLocalDescription failed\n");
            goto cleanup;
        }

        answer_buffer[answer_len] = '\0';
        if (0 != signaling_send_answer(sig, answer_buffer)) {
            printf("[webrtc] signaling_send_answer failed\n");
            goto cleanup;
        }
        printf("[webrtc] answer queued (%d bytes)\n", (int) answer_len);
    }

    printf("[webrtc] entering media/signaling pump\n");
    while (s_streaming_requested) {
        if (0 != signaling_tick(sig)) {
            printf("[webrtc] signaling_tick reported disconnect\n");
            break;
        }
        /* TODO: drain M2 shmem ring → PeerConnection_WriteFrame. */
        vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_POLL_IDLE_MS));
    }
    rc = 0;

cleanup:
    if (NULL != sig) {
        signaling_set_peer_connection(sig, NULL);
    }
    signaling_bridge.sig = NULL;
    if (peer_connection_inited) {
        (void) PeerConnection_CloseSession(&session);
    }
    if (NULL != sig) {
        signaling_disconnect(sig);
    }
    return rc;
}

static void webrtc_task(void *arg) {
    (void) arg;

    while (0 != webrtc_csprng_init()) {
        printf("[webrtc] csprng init failed, retrying\n");
        vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_BACKOFF_MS));
    }

    /* Required before any srtp_create; otherwise srtp_err_status_init_fail. */
    srtp_err_status_t srtp_rc = srtp_init();
    if (srtp_err_status_ok != srtp_rc) {
        printf("[webrtc] srtp_init failed: %d (continuing; sessions will fail)\n", (int) srtp_rc);
    }

    for (;;) {
        if (!s_streaming_requested) {
            vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_POLL_IDLE_MS));
            continue;
        }

        if (s_creds_dirty) {
            s_creds_dirty = false;
            printf("[webrtc] creds_dirty observed at session boundary\n");
        }

        int rc = run_session();
        if (!s_streaming_requested) {
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
    if (NULL == s_webrtc_task) {
        printf("[webrtc] start before init\n");
        return false;
    }
    s_streaming_requested = true;
    printf("[webrtc] starting...\n");
    return true;
}

void app_webrtc_stop(void) {
    s_streaming_requested = false;
    vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_POLL_IDLE_MS * 2U));
    printf("[webrtc] stopping\n");
}

void app_webrtc_notify_creds_updated(void) {
    s_creds_dirty = true;
}
