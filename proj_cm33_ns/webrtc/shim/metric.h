/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Avnet
 *
 * Shim stubbing out upstream awslabs `examples/metric/` telemetry surface.
 * Upstream metric.c is ~193 LOC of mutex-protected start/end timestamps with
 * a print function. Pure telemetry; nothing in the protocol path depends on
 * its output for correctness. We compile every Metric_* call to a no-op.
 *
 * The enum is preserved verbatim from upstream metric.h so that all event
 * names referenced in the copied peer_connection / signaling / ice code
 * still resolve as compile-time identifiers.
 *
 * Decision rationale captured in work/reference/UPSTREAM_CANDIDATES.md
 * under "Decided — skip: metric.{c,h}". If real on-device telemetry is
 * wanted later, replace this stub with a real implementation.
 */
#ifndef WEBRTC_SHIM_METRIC_H
#define WEBRTC_SHIM_METRIC_H

typedef enum MetricEvent
{
    METRIC_EVENT_NONE = 0,

    /* Media Events. */
    METRIC_EVENT_MEDIA_PORT_START,
    METRIC_EVENT_MEDIA_PORT_STOP,

    /* Signaling Events */
    METRIC_EVENT_SIGNALING_DESCRIBE_CHANNEL,
    METRIC_EVENT_SIGNALING_GET_ENDPOINTS,
    METRIC_EVENT_SIGNALING_GET_ICE_SERVER_LIST,
    METRIC_EVENT_SIGNALING_CONNECT_WSS_SERVER,
    METRIC_EVENT_SIGNALING_GET_CREDENTIALS,
    METRIC_EVENT_SIGNALING_JOIN_STORAGE_SESSION,

    /* ICE Events. */
    METRIC_EVENT_ICE_GATHER_HOST_CANDIDATES,
    METRIC_EVENT_ICE_GATHER_SRFLX_CANDIDATES,
    METRIC_EVENT_ICE_GATHER_RELAY_CANDIDATES,
    METRIC_EVENT_ICE_FIND_P2P_CONNECTION,

    /* Peer Connection Events. */
    METRIC_EVENT_PC_DTLS_HANDSHAKING,

    /* Combine case. */
    METRIC_EVENT_SENDING_FIRST_FRAME,

    METRIC_EVENT_MAX,
} MetricEvent_t;

static inline void Metric_Init( void ) { }
static inline void Metric_StartEvent( MetricEvent_t event ) { (void) event; }
static inline void Metric_EndEvent( MetricEvent_t event )   { (void) event; }
static inline void Metric_PrintMetrics( void ) { }
static inline void Metric_ResetEvent( void ) { }

#endif /* WEBRTC_SHIM_METRIC_H */
