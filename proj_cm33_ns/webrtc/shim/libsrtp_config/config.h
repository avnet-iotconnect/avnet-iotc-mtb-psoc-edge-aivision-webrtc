/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Avnet
 *
 * libsrtp build config for the PSoC Edge / CM33-NS port.
 *
 * libsrtp sources do `#include <config.h>` under `#ifdef HAVE_CONFIG_H`.
 * We define HAVE_CONFIG_H only for libsrtp TUs (see kvs_libs.mk: this dir
 * is on the include path for libsrtp's compile, and HAVE_CONFIG_H is set
 * project-wide -- other vendored libs don't `#include <config.h>` outside
 * an HAVE_CONFIG_H guard, so they're unaffected).
 *
 * Backend: libsrtp's internal AES (no OPENSSL/MBEDTLS/NSS/WOLFSSL backend).
 * CM33 has CPU headroom for SW-AES at our framerate (~4 fps, small NALs).
 * Revisit if SRTP perf turns tight.
 *
 * Pin: libsrtp v2.8.0 (cisco/libsrtp tag v2.8.0, commit 24b3bf8).
 */

#ifndef PROJ_CM33_NS_WEBRTC_LIBSRTP_CONFIG_H
#define PROJ_CM33_NS_WEBRTC_LIBSRTP_CONFIG_H

/* Standard headers libsrtp probes for via integers.h / alloc / etc. */
#define HAVE_STDLIB_H        1
#define HAVE_STRING_H        1
#define HAVE_INTTYPES_H      1
#define HAVE_STDINT_H        1
#define HAVE_NETINET_IN_H    1   /* lwIP POSIX-compat path supplies this */

/* Standard integer types are provided by <stdint.h> on this toolchain. */
#define HAVE_UINT8_T         1
#define HAVE_UINT16_T        1
#define HAVE_UINT32_T        1
#define HAVE_UINT64_T        1
#define HAVE_INT8_T          1
#define HAVE_INT16_T         1
#define HAVE_INT32_T         1

/* RISC target: assume slow byte access -- libsrtp picks word-aligned paths. */
#define CPU_RISC             1

/*
 * Crypto backend selectors are intentionally NOT defined:
 *   OPENSSL, MBEDTLS, NSS, WOLFSSL  -> internal AES backend.
 * AES-GCM is also not enabled (GCM undefined): DTLS-SRTP profile we use
 * (SRTP_AES128_CM_HMAC_SHA1_80) does not need it.
 */

#endif /* PROJ_CM33_NS_WEBRTC_LIBSRTP_CONFIG_H */
