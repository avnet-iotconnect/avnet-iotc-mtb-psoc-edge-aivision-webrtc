/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Avnet
 *
 * POSIX <netinet/in.h> shim that forwards to lwIP's sockets API.
 *
 * lwIP ships a similar one-line shim for <arpa/inet.h>
 * (mtb_shared/lwip/.../src/include/compat/posix/arpa/inet.h) but does NOT
 * ship one for <netinet/in.h>. libsrtp's crypto/include/datatypes.h
 * pulls <netinet/in.h> when HAVE_NETINET_IN_H is set, so we provide it
 * here. webrtc/ is on the project include path (see kvs_libs.mk).
 *
 * Same approach as the N6 reference port
 * (work/reference/iotc-stm32-n6-w6x-kvs-webrtc/Appli/Libraries/kvs_webrtc/
 *  configs/wslay/netinet/in.h).
 */
#ifndef PROJ_CM33_NS_WEBRTC_NETINET_IN_H
#define PROJ_CM33_NS_WEBRTC_NETINET_IN_H

#include "lwip/sockets.h"
#include "lwip/inet.h"

#endif /* PROJ_CM33_NS_WEBRTC_NETINET_IN_H */
