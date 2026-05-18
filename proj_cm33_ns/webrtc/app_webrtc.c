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
#include "peer_connection_sdp.h"
#include "signaling.h"
#include "stun_data_types.h"
#include "transceiver_data_types.h"

#define APP_WEBRTC_WSS_ENDPOINT_LEN 256U
#define APP_WEBRTC_AWS_REGION_MAXLEN 50U
#define APP_WEBRTC_SDP_BUF_LEN (12U * 1024U)

#define APP_WEBRTC_TRANSCEIVER_STREAM_ID "myKvsVideoStream"
#define APP_WEBRTC_TRANSCEIVER_VIDEO_TRACK_ID "myVideoTrack"
#define APP_WEBRTC_TRANSCEIVER_VIDEO_MID "0"
#define APP_WEBRTC_TRANSCEIVER_ROLLING_BUFFER_SEC 3U
#define APP_WEBRTC_TRANSCEIVER_H264_BITRATE_BPS (1400U * 1024U)

#define WEBRTC_TASK_NAME "webrtc"
#define WEBRTC_TASK_STACK_W (24U * 1024U)
#define WEBRTC_TASK_PRIO (tskIDLE_PRIORITY + 2)

#define APP_WEBRTC_POLL_IDLE_MS 20U
#define APP_WEBRTC_BACKOFF_MS 1000U
#define APP_WEBRTC_STOP_TIMEOUT_MS 5000U
#define APP_WEBRTC_LOCAL_CANDIDATE_BUF_LEN 192U

static TaskHandle_t s_webrtc_task = NULL;
static volatile bool s_start_requested = false;
static volatile bool s_creds_dirty = false;
static bool s_csprng_ready = false;
static char s_wss_endpoint[APP_WEBRTC_WSS_ENDPOINT_LEN];
static SignalingHandle s_active_signaling = NULL;
static PeerConnectionSession_t s_peer_connection_session;

typedef struct AppWebrtcSignalingBridge {
    SignalingHandle sig;
    const char *sdp_mid;
    int sdp_m_line_index;
} AppWebrtcSignalingBridge_t;


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

    // fork-aivision: current S7 flow advertises one sendonly video m-line and
    // only enables UDP host candidates in pc_config, so a single fixed
    // component/protocol candidate string is sufficient for trickle signaling.
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
    PeerConnectionSession_t *session = &s_peer_connection_session;
    AppWebrtcSignalingBridge_t signaling_bridge = {
        .sig = NULL,
        .sdp_mid = APP_WEBRTC_TRANSCEIVER_VIDEO_MID,
        .sdp_m_line_index = 0,
    };
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

    PeerConnectionSessionConfiguration_t pc_config;
    memset(&pc_config, 0, sizeof(pc_config));
    pc_config.canTrickleIce = 1U;
    pc_config.natTraversalConfigBitmap =
        ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_SEND_HOST |
        ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_ACCEPT_HOST |
        ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_ACCEPT_SRFLX;

    // fork-aivision: Keep the current integration stub aligned with the
    // product-level candidate budget in demo_config.h. For the intended use
    // case we want host + srflx as the direct-connect baseline; TURN remains
    // optional and only adds relay candidates when explicitly enabled.
    #if APP_WEBRTC_ENABLE_SRFLX
    pc_config.natTraversalConfigBitmap |= ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_SEND_SRFLX;
    #endif

    #if APP_WEBRTC_ENABLE_TURN
    pc_config.natTraversalConfigBitmap |=
        ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_SEND_RELAY |
        ICE_CANDIDATE_NAT_TRAVERSAL_CONFIG_ACCEPT_RELAY;
    #endif

    if (PEER_CONNECTION_RESULT_OK != PeerConnection_Init(session, &pc_config)) {
        printf("[webrtc] PeerConnection_Init failed\n");
        goto cleanup;
    }
    peer_connection_inited = true;

    Transceiver_t video_transceiver;
    init_video_transceiver(&video_transceiver);
    if (PEER_CONNECTION_RESULT_OK != PeerConnection_AddTransceiver(session, &video_transceiver)) {
        printf("[webrtc] PeerConnection_AddTransceiver failed\n");
        goto cleanup;
    }

    if (PEER_CONNECTION_RESULT_OK != PeerConnection_SetOnLocalCandidateReady(session, on_local_candidate_ready, &signaling_bridge)) {
        printf("[webrtc] PeerConnection_SetOnLocalCandidateReady failed\n");
        goto cleanup;
    }

    if (PEER_CONNECTION_RESULT_OK != PeerConnection_Start(session)) {
        printf("[webrtc] PeerConnection_Start failed\n");
        goto cleanup;
    }

    signaling_bridge.sig = sig;
    signaling_set_peer_connection(sig, session);

    printf("[webrtc] waiting for SDP offer...\n");
    if (0 != signaling_wait_for_offer(sig, offer_sdp, APP_WEBRTC_SDP_BUF_LEN, &offer_len)) {
        printf("[webrtc] signaling_wait_for_offer failed\n");
        goto cleanup;
    }

    size_t answer_len = PEER_CONNECTION_SDP_DESCRIPTION_BUFFER_MAX_LENGTH;
    if (0 != build_answer_from_offer(
        session,
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

    PeerConnectionBufferSessionDescription_t remote_desc;
    memset(&remote_desc, 0, sizeof(remote_desc));
    remote_desc.pSdpBuffer = offer_sdp;
    remote_desc.sdpBufferLength = offer_len;
    remote_desc.type = SDP_CONTROLLER_MESSAGE_TYPE_OFFER;

    if (PEER_CONNECTION_RESULT_OK != PeerConnection_SetRemoteDescription(session, &remote_desc)) {
        printf("[webrtc] PeerConnection_SetRemoteDescription failed\n");
        goto cleanup;
    }

    // fork-aivision: once the remote description is installed, PeerConnection
    // owns the internal ICE/timer/session-task lifecycle. The app task only
    // needs to keep the WSS signaling pump alive so trickle ICE and control
    // frames continue to flow.
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
    if (NULL != sig) {
        signaling_set_peer_connection(sig, NULL);
    }
    signaling_bridge.sig = NULL;
    if (peer_connection_inited) {
        (void) PeerConnection_CloseSession(session);
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
