################################################################################
# \file kvs_libs.mk
#
# \brief
# Build-system glue for the WebRTC / KVS third-party libraries vendored as
# git submodules under ../third_party/. Included from proj_cm33_ns/Makefile.
#
# Each library is added with three pieces:
#   SOURCES+=  -- explicit .c globs (MTB auto-discovery does not cross sibling
#                 directories outside of the project tree)
#   INCLUDES+= -- public header dirs
#   CY_IGNORE+=-- non-portable bits (CMake, host-only tests, nested .git, etc.)
#
# Layout reference (see work/reference/n6-analysis.md §15.2, §16.1, PILOT.md §3):
#   third_party/amazon-kinesis-video-streams-{stun,ice,rtp,rtcp,sdp,signaling}
#   third_party/wslay
#   third_party/libsrtp
################################################################################

THIRD_PARTY_DIR := ../third_party

# Take the in-library defaults for components that ship a
# `*_config_defaults.h` user-config indirection (SDP and SigV4). Mirrors the
# existing HTTP_DO_NOT_USE_CUSTOM_CONFIG / MQTT_DO_NOT_USE_CUSTOM_CONFIG idiom
# in this Makefile. The other KVS components (stun/ice/rtp/rtcp/signaling)
# don't ship that indirection, so no flag is needed for them.
DEFINES+=SDP_DO_NOT_USE_CUSTOM_CONFIG
DEFINES+=SIGV4_DO_NOT_USE_CUSTOM_CONFIG

# coreJSON (used by amazon-kinesis-video-streams-signaling for SDP/ICE message
# parsing). Already present in mtb_shared via aws-iot-device-sdk-embedded-C,
# but not pulled into the build by anything else today, so we add it here.
CORE_JSON_DIR := ../../mtb_shared/aws-iot-device-sdk-embedded-C/202103.00/libraries/standard/coreJSON
SOURCES+=$(CORE_JSON_DIR)/source/core_json.c
INCLUDES+=$(CORE_JSON_DIR)/source/include

# -----------------------------------------------------------------------------
# amazon-kinesis-video-streams-stun
# -----------------------------------------------------------------------------
SOURCES+=$(wildcard $(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-stun/source/*.c)
INCLUDES+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-stun/source/include
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-stun/test
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-stun/CMakeLists.txt
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-stun/stunFilePaths.cmake

# -----------------------------------------------------------------------------
# amazon-kinesis-video-streams-ice
# Depends on stun headers (flat include path: #include "stun_*.h").
# Upstream's source/dependency/amazon-kinesis-video-streams-stun/ is an empty
# nested submodule placeholder (`update = none`) -- ignore it; we already have
# stun at the top level above.
# -----------------------------------------------------------------------------
SOURCES+=$(wildcard $(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-ice/source/*.c)
INCLUDES+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-ice/source/include
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-ice/source/dependency
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-ice/test
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-ice/CMakeLists.txt
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-ice/iceFilePaths.cmake

# -----------------------------------------------------------------------------
# amazon-kinesis-video-streams-rtp
# Codec packetizers live in codec_packetizers/<codec>/. We pull H.264 only;
# G.711, H.265, Opus, VP8 are not used (video-only, H.264-only per PILOT.md §6).
# -----------------------------------------------------------------------------
SOURCES+=$(wildcard $(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtp/source/*.c)
SOURCES+=$(wildcard $(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtp/codec_packetizers/h264/*.c)
INCLUDES+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtp/source/include
INCLUDES+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtp/codec_packetizers/h264/include
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtp/codec_packetizers/g711
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtp/codec_packetizers/h265
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtp/codec_packetizers/opus
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtp/codec_packetizers/vp8
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtp/test
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtp/CMakeLists.txt
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtp/rtpFilePaths.cmake

# -----------------------------------------------------------------------------
# amazon-kinesis-video-streams-rtcp
# -----------------------------------------------------------------------------
SOURCES+=$(wildcard $(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtcp/source/*.c)
INCLUDES+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtcp/source/include
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtcp/test
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtcp/CMakeLists.txt
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-rtcp/rtcpFilePaths.cmake

# -----------------------------------------------------------------------------
# amazon-kinesis-video-streams-sdp
# -----------------------------------------------------------------------------
SOURCES+=$(wildcard $(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-sdp/source/*.c)
INCLUDES+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-sdp/source/include
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-sdp/test
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-sdp/CMakeLists.txt
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-sdp/sdpFilePaths.cmake

# -----------------------------------------------------------------------------
# amazon-kinesis-video-streams-signaling
# Depends on coreJSON (added above).
# Empty nested coreJSON placeholder (`update = none`) is ignored.
# -----------------------------------------------------------------------------
SOURCES+=$(wildcard $(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-signaling/source/*.c)
INCLUDES+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-signaling/source/include
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-signaling/source/dependency
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-signaling/test
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-signaling/CMakeLists.txt
CY_IGNORE+=$(THIRD_PARTY_DIR)/amazon-kinesis-video-streams-signaling/signalingFilePaths.cmake

# -----------------------------------------------------------------------------
# SigV4-for-AWS-IoT-embedded-sdk (v1.3.1)
# Used by signaling.c to sign the ConnectAsViewer WSS URL with the 1-hour AWS
# creds triplet (AKID, secret, session token).  Defaults config taken via
# SIGV4_DO_NOT_USE_CUSTOM_CONFIG (set above).
# -----------------------------------------------------------------------------
SIGV4_DIR := $(THIRD_PARTY_DIR)/SigV4-for-AWS-IoT-embedded-sdk
SOURCES+=$(wildcard $(SIGV4_DIR)/source/*.c)
INCLUDES+=$(SIGV4_DIR)/source/include
CY_IGNORE+=$(SIGV4_DIR)/test
CY_IGNORE+=$(SIGV4_DIR)/tools
CY_IGNORE+=$(SIGV4_DIR)/docs
CY_IGNORE+=$(SIGV4_DIR)/CMakeLists.txt
CY_IGNORE+=$(SIGV4_DIR)/sigv4FilePaths.cmake

# -----------------------------------------------------------------------------
# wslay (WebSocket frame codec used by KVS WSS signaling)
# Autoconf bypassed: we hand-pick the 5 source files in lib/ and provide the
# config.h.in flags via DEFINES.  HAVE_ARPA_INET_H makes wslay_net.h pull
# <arpa/inet.h>; lwIP's POSIX-compat path supplies that header (it forwards to
# lwIP's htonl/ntohl).  WORDS_BIGENDIAN is left undefined -> Cortex-M is
# little-endian, so wslay's runtime byteswap path kicks in (correct).
# -----------------------------------------------------------------------------
WSLAY_DIR    := $(THIRD_PARTY_DIR)/wslay
LWIP_POSIX   := ../../mtb_shared/lwip/STABLE-2_1_2_RELEASE/src/include/compat/posix

SOURCES+=$(wildcard $(WSLAY_DIR)/lib/*.c)
INCLUDES+=$(WSLAY_DIR)/lib
INCLUDES+=$(WSLAY_DIR)/lib/includes
INCLUDES+=$(LWIP_POSIX)
DEFINES+=HAVE_ARPA_INET_H
# wslay.h includes <wslay/wslayver.h> which is generated by autoconf from
# wslayver.h.in.  Define WSLAY_VERSION up-front to skip that include
# (documented escape hatch in wslay.h).  Pin matches third_party/wslay submodule.
DEFINES+=WSLAY_VERSION=\"1.1.1\"

CY_IGNORE+=$(WSLAY_DIR)/examples
CY_IGNORE+=$(WSLAY_DIR)/tests
CY_IGNORE+=$(WSLAY_DIR)/doc
CY_IGNORE+=$(WSLAY_DIR)/m4
CY_IGNORE+=$(WSLAY_DIR)/cmake
CY_IGNORE+=$(WSLAY_DIR)/CMakeLists.txt
CY_IGNORE+=$(WSLAY_DIR)/lib/CMakeLists.txt
CY_IGNORE+=$(WSLAY_DIR)/lib/config.h.in
CY_IGNORE+=$(WSLAY_DIR)/configure.ac
CY_IGNORE+=$(WSLAY_DIR)/Makefile.am
CY_IGNORE+=$(WSLAY_DIR)/lib/Makefile.am

# -----------------------------------------------------------------------------
# libsrtp (v2.8.0) -- SRTP/SRTCP encryption for media path
# Backend: libsrtp's internal AES (OPENSSL/MBEDTLS/NSS/WOLFSSL all undefined,
# see webrtc/config.h).  CM33 has CPU headroom for SW-AES at our framerate.
# Profile used: SRTP_AES128_CM_HMAC_SHA1_80 -> AES-GCM (GCM macro) not needed.
#
# config.h: shared `webrtc/config.h` is project-owned; HAVE_CONFIG_H is defined
# project-wide.  Other vendored libs (wslay, KVS protocol libs) either guard
# their `#include <config.h>` with `#ifdef HAVE_CONFIG_H` and don't read any
# macro that would collide, or bypass HAVE_CONFIG_H entirely (wslay).
#
# CY_IGNORE strategy: blanket-ignore the whole tree, then re-add only the
# files we actually want via SOURCES+= (cleaner than enumerating every
# unwanted dir like fuzzer/, test/, timing/, doc/, cmake/, etc.).  This is
# different from the awslabs KVS libs above where `source/` is the only
# *.c-bearing dir; libsrtp scatters .c files across srtp/, crypto/cipher/,
# crypto/hash/, crypto/kernel/, crypto/math/, crypto/replay/.
# -----------------------------------------------------------------------------
LIBSRTP_DIR := $(THIRD_PARTY_DIR)/libsrtp

# Tell libsrtp to consume our config header.
# `./webrtc` puts webrtc/config.h on the include path AND supplies our
# webrtc/netinet/in.h shim (lwIP doesn't ship that POSIX header even though
# it ships <arpa/inet.h>; libsrtp's datatypes.h needs it for htonX/ntohX).
DEFINES+=HAVE_CONFIG_H
INCLUDES+=./webrtc

# Demote `incompatible-pointer-types` from error to warning. libsrtp's srtp.c
# passes `(unsigned int *)` where the cipher API takes `uint32_t *`. Both are
# 32-bit on Cortex-M but the types differ (newlib defines uint32_t as
# `unsigned long`), and modern GCC errors on this by default. Same flag the
# N6 reference port uses (.cproject; see PILOT.md / kvs_libs.mk history).
# Scope is whole-build, but the warning is benign for non-libsrtp code.
CFLAGS+=-Wno-error=incompatible-pointer-types

# srtp.c -- the only .c under srtp/.
SOURCES+=$(LIBSRTP_DIR)/srtp/srtp.c

# crypto/cipher/: pick the internal-AES + null variants; skip backend shims
# (OpenSSL/NSS/MbedTLS) and AES-GCM (we don't negotiate GCM).
SOURCES+=$(LIBSRTP_DIR)/crypto/cipher/cipher.c
SOURCES+=$(LIBSRTP_DIR)/crypto/cipher/cipher_test_cases.c
SOURCES+=$(LIBSRTP_DIR)/crypto/cipher/aes.c
SOURCES+=$(LIBSRTP_DIR)/crypto/cipher/aes_icm.c
SOURCES+=$(LIBSRTP_DIR)/crypto/cipher/null_cipher.c

# crypto/hash/: HMAC-SHA1 (internal) + null; skip OpenSSL/NSS/MbedTLS shims.
SOURCES+=$(LIBSRTP_DIR)/crypto/hash/auth.c
SOURCES+=$(LIBSRTP_DIR)/crypto/hash/auth_test_cases.c
SOURCES+=$(LIBSRTP_DIR)/crypto/hash/hmac.c
SOURCES+=$(LIBSRTP_DIR)/crypto/hash/sha1.c
SOURCES+=$(LIBSRTP_DIR)/crypto/hash/null_auth.c

# crypto/kernel/, crypto/math/, crypto/replay/: take everything.
SOURCES+=$(wildcard $(LIBSRTP_DIR)/crypto/kernel/*.c)
SOURCES+=$(LIBSRTP_DIR)/crypto/math/datatypes.c
SOURCES+=$(wildcard $(LIBSRTP_DIR)/crypto/replay/*.c)

# Public + crypto-internal headers.
INCLUDES+=$(LIBSRTP_DIR)/include
INCLUDES+=$(LIBSRTP_DIR)/crypto/include

# Ignore everything that isn't explicitly listed above (build infrastructure,
# host-only tests, fuzzers, docs, alternate backends, RFC 7714 GCM dirs, etc.).
CY_IGNORE+=$(LIBSRTP_DIR)/cmake
CY_IGNORE+=$(LIBSRTP_DIR)/doc
CY_IGNORE+=$(LIBSRTP_DIR)/fuzzer
CY_IGNORE+=$(LIBSRTP_DIR)/test
CY_IGNORE+=$(LIBSRTP_DIR)/timing
CY_IGNORE+=$(LIBSRTP_DIR)/CMakeLists.txt
CY_IGNORE+=$(LIBSRTP_DIR)/Config.cmake.in
CY_IGNORE+=$(LIBSRTP_DIR)/Makefile.in
CY_IGNORE+=$(LIBSRTP_DIR)/configure
CY_IGNORE+=$(LIBSRTP_DIR)/configure.ac
CY_IGNORE+=$(LIBSRTP_DIR)/meson.build
CY_IGNORE+=$(LIBSRTP_DIR)/meson_options.txt
# Backend variants we do not compile.
CY_IGNORE+=$(LIBSRTP_DIR)/crypto/cipher/aes_gcm_mbedtls.c
CY_IGNORE+=$(LIBSRTP_DIR)/crypto/cipher/aes_gcm_nss.c
CY_IGNORE+=$(LIBSRTP_DIR)/crypto/cipher/aes_gcm_ossl.c
CY_IGNORE+=$(LIBSRTP_DIR)/crypto/cipher/aes_icm_mbedtls.c
CY_IGNORE+=$(LIBSRTP_DIR)/crypto/cipher/aes_icm_nss.c
CY_IGNORE+=$(LIBSRTP_DIR)/crypto/cipher/aes_icm_ossl.c
CY_IGNORE+=$(LIBSRTP_DIR)/crypto/hash/hmac_mbedtls.c
CY_IGNORE+=$(LIBSRTP_DIR)/crypto/hash/hmac_nss.c
CY_IGNORE+=$(LIBSRTP_DIR)/crypto/hash/hmac_ossl.c
