/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Avnet
 *
 * Shared build-time config header for vendored WebRTC third-party libraries
 * that follow the autoconf `#ifdef HAVE_CONFIG_H` / `#include <config.h>`
 * convention.
 *
 * Why one shared file: each contributing library only consults the macros
 * it cares about, so co-locating them in one project-owned header keeps
 * the wiring visible in a single place. Sections below are tagged with
 * the library they apply to. Add a new section when wiring a new lib;
 * don't fork into per-library config.h files unless the macro names
 * collide.
 *
 * Active consumers:
 *   - libsrtp v2.8.0 (cisco/libsrtp). HAVE_CONFIG_H is defined for the
 *     whole project (see kvs_libs.mk). Other vendored libs guard their
 *     `#include <config.h>` with `#ifdef HAVE_CONFIG_H` too -- the macros
 *     they read are listed alongside libsrtp's where they overlap, or in
 *     their own section below.
 *
 *   - wslay (currently bypasses HAVE_CONFIG_H -- macros passed via -D in
 *     kvs_libs.mk). If a future wslay tweak needs this header, add a
 *     section here.
 */

#ifndef PROJ_CM33_NS_WEBRTC_CONFIG_H
#define PROJ_CM33_NS_WEBRTC_CONFIG_H

/* ------------------------------------------------------------------ */
/* libsrtp v2.8.0                                                      */
/* ------------------------------------------------------------------ */
/*
 * Backend: libsrtp's internal AES (OPENSSL / MBEDTLS / NSS / WOLFSSL
 * intentionally undefined). CM33 has CPU headroom for SW-AES at our
 * framerate (~4 fps, small NALs). Revisit if SRTP perf turns tight.
 *
 * AES-GCM (GCM macro) is also undefined: the DTLS-SRTP profile we
 * negotiate (SRTP_AES128_CM_HMAC_SHA1_80) does not need it.
 */

/* Standard headers libsrtp probes for via integers.h / alloc.c. */
#define HAVE_STDLIB_H        1
#define HAVE_STRING_H        1
#define HAVE_INTTYPES_H      1
#define HAVE_STDINT_H        1
#define HAVE_NETINET_IN_H    1   /* via lwIP POSIX-compat include path */

/*
 * lwIP's LWIP_TIMEVAL_PRIVATE=0 is set globally via wifi-core's lwipopts.h
 * (mtb_shared/wifi-core-freertos-lwip-mbedtls/.../configs/lwipopts.h:101).
 * That makes lwip/sockets.h defer to newlib's <sys/time.h> for `struct
 * timeval`. SNTP relies on the same setup and works -- nothing to do here.
 */

/* Standard integer types are supplied by <stdint.h> on this toolchain. */
#define HAVE_UINT8_T         1
#define HAVE_UINT16_T        1
#define HAVE_UINT32_T        1
#define HAVE_UINT64_T        1
#define HAVE_INT8_T          1
#define HAVE_INT16_T         1
#define HAVE_INT32_T         1

/* RISC target: assume slow byte access -- libsrtp picks word-aligned paths. */
#define CPU_RISC             1

/* sizeof checks used by integers.h to pick the uint64_t typedef path. */
#define SIZEOF_UNSIGNED_LONG        4
#define SIZEOF_UNSIGNED_LONG_LONG   8

/* Compiler supports `inline` -- skip libsrtp's `#define inline` fallback. */
#define HAVE_INLINE          1

/*
 * Package identity strings consumed by srtp_get_version_string() and
 * srtp_get_version() (srtp/srtp.c). Pin matches the submodule.
 */
#define PACKAGE_STRING       "libsrtp 2.8.0"
#define PACKAGE_VERSION      "2.8.0"

/*
 * Note on debug logging: libsrtp's `debug_print*` macros are defined in
 * crypto/include/err.h and expand to calls into srtp_err_report() (defined
 * in crypto/kernel/err.c, which we compile). They're gated at runtime by
 * `mod.on` flags that default to off, so they're cheap. We don't stub them
 * here -- the AWS Ameba/N6 configs do, but their stubs are silently
 * overridden by err.h's later definitions anyway, so the stubs were always
 * a no-op.
 */

/*
 * Map libsrtp's malloc/free onto the FreeRTOS heap (pvPortMalloc / vPortFree).
 * Two reasons:
 *   - libsrtp's allocations are SRTP session state -- they live in the
 *     same lifetime envelope as the rest of WebRTC, which is FreeRTOS-
 *     heap territory.  Co-locating means one heap to budget instead of
 *     two (newlib heap vs FreeRTOS heap).
 *   - Future-proof against a swap to a tracked/instrumented FreeRTOS heap.
 *
 * `malloc`/`free` are C-library names; the redefine lands inside libsrtp
 * TUs (which include this config.h via HAVE_CONFIG_H) and does NOT leak
 * to other code (other vendored libs don't include this header, and our
 * own webrtc glue won't `#include "config.h"` -- it'll just call malloc).
 */
#include "FreeRTOS.h"
#define malloc(sz)   pvPortMalloc(sz)
#define free(p)      vPortFree(p)

#endif /* PROJ_CM33_NS_WEBRTC_CONFIG_H */
