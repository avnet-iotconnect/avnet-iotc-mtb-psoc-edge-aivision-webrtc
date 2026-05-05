/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

#ifndef WEBRTC_AWS_CREDS_H_
#define WEBRTC_AWS_CREDS_H_

/* The 1-hour AWS triplet + per-channel info that the IoTC discovery flow
 * yields. Held in app_webrtc.c, consumed by signaling.c (SigV4 signing).
 *
 * All fields point at storage owned elsewhere (today: string literals from
 * #defines in app_webrtc.c). The struct is read-only after populate. */
typedef struct AwsCreds {
    const char *region;             /* e.g. "us-east-1" */
    const char *channel_arn;        /* full ARN of the KVS signaling channel */
    const char *client_id;          /* X-Amz-ClientId for ConnectAsViewer; per-device unique */
    const char *access_key_id;
    const char *secret_access_key;
    const char *session_token;      /* may be NULL only if using long-term creds (not our case) */
} AwsCreds;

#endif /* WEBRTC_AWS_CREDS_H_ */
