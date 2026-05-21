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
#define WEBRTC_TASK_STACK_W (110U * 1024U)
#define WEBRTC_TASK_PRIO (tskIDLE_PRIORITY + 2)

#define APP_WEBRTC_POLL_IDLE_MS 20U
#define APP_WEBRTC_BACKOFF_MS 1000U
#define APP_WEBRTC_LOCAL_CANDIDATE_BUF_LEN 192U

// File-scope state is deliberately minimal — only what truly outlives a
// session or crosses task boundaries lives here. Everything per-session
// (PC session struct, signaling handle, WSS endpoint, SDP buffers) is local
// to run_session.
//
// Streaming semantics: app_webrtc_start sets s_streaming_requested = true,
// app_webrtc_stop clears it. The media pump loop in run_session observes the
// flag and exits broadcasting when it goes false; the surrounding session
// then tears down normally. We never interrupt mid-setup or mid-negotiation —
// "stop" is honored only at the next loop tick. If stop arrives before the
// offer, the session keeps waiting until the offer arrives or the network
// drops.
static TaskHandle_t s_webrtc_task = NULL;             // task handle, process-lifetime
static volatile bool s_streaming_requested = false;     // start/stop signal: should we be running the task?
static volatile bool s_creds_dirty = false;           // refresh hook → session boundary

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

// Build the SigV4-signed WSS URL and open the signaling websocket. The signed
// URL is variable-length so it lives on the heap, owned by this function and
// freed before return. Caller never sees the buffer.
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

static int run_session(void) {
    // Per-session locals. Everything lives in this stack frame; nothing
    // survives when the function returns. The PeerConnection struct in
    // particular leaves BSS — it's reclaimed between sessions.
    char region_buf[APP_WEBRTC_AWS_REGION_MAXLEN + 1U];
    char wss_endpoint[APP_WEBRTC_WSS_ENDPOINT_LEN];
    AwsCreds aws_creds;
    PeerConnectionSession_t session = {0};
    SignalingHandle sig = NULL;
    AppWebrtcSignalingBridge_t signaling_bridge = {
        .sig = NULL,
        .sdp_mid = APP_WEBRTC_TRANSCEIVER_VIDEO_MID,
        .sdp_m_line_index = 0,
    };
    bool peer_connection_inited = false;
    int rc = -1;

    // Phase: fetch fresh creds and resolve the WSS endpoint. Creds live in
    // the IoTC SDK cache; we read pointers into it (refresh hook flips
    // s_creds_dirty when the SDK rotates them — already observed at the
    // session boundary in webrtc_task).
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

    // Phase: peer connection init + start ICE gathering. UDP host socket opens
    // inside the ICE controller during PeerConnection_Start; the first local
    // candidate fires `on_local_candidate_ready` against `signaling_bridge`,
    // which lives at function scope so it outlives the peer connection.
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

        Transceiver_t video_transceiver;
        init_video_transceiver(&video_transceiver);
        if (PEER_CONNECTION_RESULT_OK != PeerConnection_AddTransceiver(&session, &video_transceiver)) {
            printf("[webrtc] PeerConnection_AddTransceiver failed\n");
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

    // Phase: SDP offer/answer. Canonical upstream flow:
    //   SetRemoteDescription → CreateAnswer → SetLocalDescription → send.
    // SetRemoteDescription copies the offer into session->remoteSdpBuffer
    // (peer_connection.c ~line 1791), inits DTLS, and starts the ICE
    // controller, so offer_sdp can drop out of scope before CreateAnswer.
    {
        char offer_sdp[APP_WEBRTC_SDP_BUF_LEN]; // 12 KB
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
    } // offer_sdp dies here (~12 KB freed); offer now lives in session->remoteSdpBuffer

    {
        char local_desc_buffer[PEER_CONNECTION_SDP_DESCRIPTION_BUFFER_MAX_LENGTH];  // 10 KB
        char answer_buffer[PEER_CONNECTION_SDP_DESCRIPTION_BUFFER_MAX_LENGTH + 1U]; // 10 KB
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
    } // local_desc_buffer + answer_buffer die here (~20 KB freed)

    // Phase: media + signaling pump. No SDP-sized buffers from here on. ICE
    // checks → DTLS handshake → SRTP keying run inside PeerConnection's
    // internal tasks. The app task only keeps signaling alive (trickle ICE)
    // and — once S10 lands — feeds H.264 NALs into PeerConnection_WriteFrame.
    printf("[webrtc] entering media/signaling pump\n");
    while (s_streaming_requested) {
        if (0 != signaling_tick(sig)) {
            printf("[webrtc] signaling_tick reported disconnect\n");
            break;
        }
        // S10 stub: drain_media_ring_step(&session);  // M2 shmem → PeerConnection_WriteFrame
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

    // One-shot crypto init. Retry with backoff if the entropy source isn't
    // ready yet — eventually succeeds, then we drop into the session loop.
    while (0 != webrtc_csprng_init()) {
        printf("[webrtc] csprng init failed, retrying\n");
        vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_BACKOFF_MS));
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

// Enable broadcasting. Pure signal: flip the flag and return. Creds/endpoint
// validation lives in run_session, where transient failures roll into the
// retry loop.
bool app_webrtc_start(void) {
    if (NULL == s_webrtc_task) {
        printf("[webrtc] start before init\n");
        return false;
    }
    s_streaming_requested = true;
    printf("[webrtc] starting...\n");
    return true;
}

// Disable broadcasting. The media pump loop in run_session observes
// s_streaming_requested and exits when it goes false; session cleanup proceeds
// normally afterward. No mid-setup interruption — honored on next loop tick.
void app_webrtc_stop(void) {
    s_streaming_requested = false;
    vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_POLL_IDLE_MS * 2U));
    printf("[webrtc] stopping\n");
}

void app_webrtc_notify_creds_updated(void) {
    s_creds_dirty = true;
}
