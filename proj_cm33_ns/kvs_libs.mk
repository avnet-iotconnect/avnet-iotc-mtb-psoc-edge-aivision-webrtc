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
# Layout reference (see work/reference/n6-analysis.md §15.2, §16.1, PLAN.md §1):
#   third_party/amazon-kinesis-video-streams-{stun,ice,rtp,rtcp,sdp,signaling}
#   third_party/libsrtp        -- not yet wired (see TODO at bottom)
#   third_party/wslay          -- not yet wired (see TODO at bottom)
################################################################################

THIRD_PARTY_DIR := ../third_party

# Take the in-library defaults for the SDP component (skips the
# `#include "sdp_config.h"` user-config indirection). Mirrors the existing
# HTTP_DO_NOT_USE_CUSTOM_CONFIG / MQTT_DO_NOT_USE_CUSTOM_CONFIG idiom in this
# Makefile. The other KVS components (stun/ice/rtp/rtcp/signaling) don't ship
# a *_config_defaults.h indirection, so no flag is needed for them.
# This matches what the N6 reference project does (.cproject sets only
# SDP_DO_NOT_USE_CUSTOM_CONFIG; no custom config headers are written for the
# other KVS components -- defaults are taken).
DEFINES+=SDP_DO_NOT_USE_CUSTOM_CONFIG

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
# TODO (next turn): libsrtp
# -----------------------------------------------------------------------------
# third_party/libsrtp (pinned v2.8.0) is checked out but NOT wired into the
# build yet.  Decision (PILOT.md / PLAN.md): use libsrtp's internal AES
# backend, no mbedTLS-crypto shim.  Owner accepted the SW-AES perf hit (CM33
# has plenty of headroom for line-rate SRTP at our framerate).  If perf turns
# tight later we can revisit and add an mbedTLS cipher adapter or call the
# Cypress crypto block directly.
#
# Wiring sketch:
#   - Hand-authored libsrtp_config.h with: PACKAGE_STRING, HAVE_STDLIB_H,
#     HAVE_STRING_H, HAVE_INTTYPES_H, HAVE_STDINT_H, HAVE_NETINET_IN_H
#     (via lwIP POSIX compat), CPU_RISC, OPENSSL undefined, MBEDTLS undefined,
#     NSS undefined, WOLFSSL undefined.  Place under proj_cm33_ns/configs/.
#   - SOURCES globs: libsrtp/srtp/*.c, libsrtp/crypto/cipher/{cipher.c,
#     cipher_test_cases.c,aes.c,aes_icm.c,null_cipher.c}, libsrtp/crypto/hash/
#     {auth.c,auth_test_cases.c,hmac.c,sha1.c,null_auth.c},
#     libsrtp/crypto/kernel/*.c, libsrtp/crypto/math/datatypes.c,
#     libsrtp/crypto/replay/*.c.
#     Skip OpenSSL/NSS/WolfSSL/MbedTLS variants under crypto/cipher/ and
#     crypto/hash/.
#   - INCLUDES: libsrtp/include, libsrtp/crypto/include.
#   - DEFINES: HAVE_CONFIG_H (so libsrtp picks up our config.h).
