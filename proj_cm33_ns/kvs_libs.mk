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
# TODO (next turn): libsrtp, wslay
# -----------------------------------------------------------------------------
# third_party/libsrtp (pinned v2.8.0) and third_party/wslay (pinned
# release-1.1.1) are checked out but NOT wired into the build yet. Both have
# autoconf/CMake build systems we'll bypass; both need a small hand-authored
# config header.
#
# libsrtp specifics:
#   - Needs HAVE_CONFIG_H + a config.h (or build flags). Crypto backend must
#     be selected -- we want mbedtls. Upstream supports openssl/nss/wolfssl
#     directly; mbedtls integration is not built-in and may need a small shim
#     OR we use libsrtp's internal AES (--enable-openssl=no path) since
#     DTLS-SRTP key derivation happens outside libsrtp anyway.
#   - Source globs: libsrtp/srtp/*.c, libsrtp/crypto/cipher/*.c (subset),
#     libsrtp/crypto/hash/*.c (subset), libsrtp/crypto/kernel/*.c,
#     libsrtp/crypto/math/*.c, libsrtp/crypto/replay/*.c.
#     Need to CY_IGNORE openssl/nss/wolfssl variants and tests.
#
# wslay specifics:
#   - Autoconf-based; we bypass and pick sources directly: wslay/lib/*.c.
#   - Needs config.h with HAVE_ARPA_INET_H, HAVE_NETINET_IN_H, sizeof macros.
#     Build a thin platform shim: wslay assumes BSD sockets, htonl/ntohl --
#     lwIP provides these.
#   - Public headers in wslay/lib/includes/wslay/.
