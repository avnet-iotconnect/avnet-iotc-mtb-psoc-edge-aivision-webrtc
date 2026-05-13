/*
 * fork-aivision: minimal port-config header (upstream calls this "demo_config.h").
 *
 * Upstream's examples/ tree ships demo_config_template.h with the convention
 * that each downstream port copies it to demo_config.h and fills in values.
 * This file is our copy, intentionally trimmed.
 *
 * What lives here:
 *   - Compile-time knobs the algorithm tier conditionally compiles against
 *     (#if ENABLE_TWCC_SUPPORT in peer_connection_data_types.h, etc.).
 *   - Stubs for codec/audio selection so upstream's "exactly one must be set"
 *     #error checks don't fire when their headers are pulled in transitively.
 *
 * What does NOT live here (and must not be added later):
 *   AWS_REGION, AWS_KVS_CHANNEL_NAME, AWS_CA_CERT_PEM,
 *   AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_SESSION_TOKEN,
 *   AWS_IOT_THING_NAME, AWS_IOT_THING_ROLE_ALIAS,
 *   AWS_IOT_THING_CERT, AWS_IOT_THING_PRIVATE_KEY,
 *   AWS_CREDENTIALS_ENDPOINT, AWS_KVS_AGENT_NAME.
 *
 * Upstream's app_common.c bakes these in at compile time. We don't build
 * app_common.c -- our orchestrator (app_webrtc.c) receives region / channel
 * name / AWS creds from IoTC discovery at runtime, and the CA cert comes from
 * iotcl_certs.h. If a future file pulled in from upstream references one of
 * these names as a #define, do NOT add it here -- replace the use with a
 * runtime value reached through the orchestrator.
 */

#ifndef DEMO_CONFIG_H
#define DEMO_CONFIG_H

/* Algorithm-tier compile-time knobs (load-bearing). */
#define ENABLE_TWCC_SUPPORT   1U
#define AWS_MAX_VIEWER_NUM    ( 2 )

/* Codec / audio selection.
 *
 * Audio is out of scope for this project (no microphone on the board). These
 * defines exist only to satisfy upstream's "exactly one audio format" and
 * "exactly one video codec" #error checks where they sit in files we may
 * transitively pull in. AUDIO_OPUS=1 is an arbitrary pick to satisfy the
 * single-bit constraint; no audio path is wired.
 *
 * TODO (debt rework, PILOT S12 territory): weed out audio-related code from
 * the copied upstream files entirely, then drop these defines. Tracked
 * separately so it doesn't block the build-up.
 */
#define USE_VIDEO_CODEC_H264          1
#define USE_VIDEO_CODEC_H265          0
#define AUDIO_G711_MULAW              0
#define AUDIO_G711_ALAW               0
#define AUDIO_OPUS                    1
#define MEDIA_PORT_ENABLE_AUDIO_RECV  0
#define JOIN_STORAGE_SESSION          0

#endif /* DEMO_CONFIG_H */
