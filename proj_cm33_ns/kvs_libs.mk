################################################################################
# \file kvs_libs.mk
#
# \brief
# Build-system glue for the WebRTC / KVS third-party libraries. These are now
# vendored as .mtb dependencies (deps/*.mtb) and live in mtb_shared, so MTB's
# library auto-discovery (SEARCH_MTB_MK, populated by libs/mtb.mk) pulls their
# sources into the build and puts every directory on the include path for us.
#
# That flips the job of this file from the old git-submodule layout: instead of
# explicitly listing each library's SOURCES/INCLUDES, we only have to:
#   CY_IGNORE+= -- carve OUT the bits auto-discovery would otherwise build but
#                  we don't want (host-only tests, CMake/autoconf glue, nested
#                  submodule placeholders, alternate crypto backends, the vp8
#                  packetizer, and any upstream source we override with a
#                  patched copy under ./webrtc/patches/).
#   DEFINES+=   -- real compile-time config.
# Paths are anchored on the per-library $(SEARCH_<name>) variables from
# libs/mtb.mk, so they track whatever commit/tag the .mtb pins.
#
# Layout reference (see work/reference/n6-analysis.md §15.2, §16.1, PILOT.md §3):
#   amazon-kinesis-video-streams-{stun,ice,rtp,rtcp,sdp,signaling}
#   wslay / libsrtp / SigV4-for-AWS-IoT-embedded-sdk
################################################################################

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
# (signaling ships its own nested coreJSON placeholder under source/dependency
# which we CY_IGNORE below, so this is the single canonical copy.)
CORE_JSON_DIR := ../../mtb_shared/aws-iot-device-sdk-embedded-C/202103.00/libraries/standard/coreJSON
SOURCES+=$(CORE_JSON_DIR)/source/core_json.c
INCLUDES+=$(CORE_JSON_DIR)/source/include

# -----------------------------------------------------------------------------
# WebRTC tier (project-owned glue + patch overlays, auto-discovered under
# ./webrtc/). See proj_cm33_ns/webrtc/README.md for the tier layout. ./webrtc
# is also where the project-owned config.h and the netinet/in.h shim live (used
# by libsrtp below); it is on the include path via auto-discovery, but we keep
# it explicit here because the config.h resolution is load-bearing.
# -----------------------------------------------------------------------------
INCLUDES+=./webrtc
INCLUDES+=./webrtc/shim

# Files temporarily excluded during the pivot build-up.
#   - media_source_ring* → S10 (media adapter rewrite against upstream's
#                          AppMediaSource_Init-style callback contract)
CY_IGNORE+=./webrtc/media_source_ring.c
CY_IGNORE+=./webrtc/media_source_ring.h

# -----------------------------------------------------------------------------
# amazon-kinesis-video-streams-stun
# -----------------------------------------------------------------------------
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-stun)/test
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-stun)/CMakeLists.txt
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-stun)/stunFilePaths.cmake

# -----------------------------------------------------------------------------
# amazon-kinesis-video-streams-ice
# Depends on stun headers (flat include path: #include "stun_*.h").
# source/dependency/ holds a nested amazon-kinesis-video-streams-stun copy;
# ignore it so auto-discovery doesn't build a second stun and collide with the
# top-level one above.
# -----------------------------------------------------------------------------
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-ice)/source/dependency
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-ice)/test
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-ice)/CMakeLists.txt
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-ice)/iceFilePaths.cmake

# -----------------------------------------------------------------------------
# amazon-kinesis-video-streams-rtp
# Codec packetizers live in codec_packetizers/<codec>/. The upstream
# peer_connection code references all four codec helpers (h264, h265, opus,
# g711) unconditionally -- dead branches at runtime since we only negotiate
# H.264, but they must link, so we let auto-discovery keep them. vp8 is the
# exception: its *.c expect headers under a dir we don't ship, so ignore it.
# -----------------------------------------------------------------------------
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-rtp)/codec_packetizers/vp8
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-rtp)/test
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-rtp)/CMakeLists.txt
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-rtp)/rtpFilePaths.cmake

# -----------------------------------------------------------------------------
# amazon-kinesis-video-streams-rtcp
# -----------------------------------------------------------------------------
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-rtcp)/test
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-rtcp)/CMakeLists.txt
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-rtcp)/rtcpFilePaths.cmake

# -----------------------------------------------------------------------------
# amazon-kinesis-video-streams-sdp
# We override both upstream sources (sdp_serializer.c / sdp_deserializer.c) with
# patched copies under ./webrtc/patches/sdp/ (newlib-nano has no %llu, so they
# use u64_to_dec/dec_to_u64 instead of printf/scanf for the long-long path --
# see that dir's README). Ignore the upstream .c so the patched copies are the
# only ones built; keep source/include on the path for the headers.
# -----------------------------------------------------------------------------
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-sdp)/source/sdp_serializer.c
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-sdp)/source/sdp_deserializer.c
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-sdp)/test
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-sdp)/CMakeLists.txt
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-sdp)/sdpFilePaths.cmake

# -----------------------------------------------------------------------------
# amazon-kinesis-video-streams-signaling
# Depends on coreJSON (added above). source/dependency/ holds a nested coreJSON
# placeholder; ignore it so the canonical coreJSON above is the only copy.
# -----------------------------------------------------------------------------
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-signaling)/source/dependency
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-signaling)/test
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-signaling)/CMakeLists.txt
CY_IGNORE+=$(SEARCH_amazon-kinesis-video-streams-signaling)/signalingFilePaths.cmake

# -----------------------------------------------------------------------------
# SigV4-for-AWS-IoT-embedded-sdk
# Used by signaling.c to sign the ConnectAsMaster WSS URL with the 1-hour AWS
# creds triplet (AKID, secret, session token). Defaults config taken via
# SIGV4_DO_NOT_USE_CUSTOM_CONFIG (set above). The processing buffer holds the
# full canonical request including the session token (~1200 chars for STS), so
# the 1024 default isn't enough -- bump to 2048 (matches reference KVS projects).
# -----------------------------------------------------------------------------
DEFINES+=SIGV4_PROCESSING_BUFFER_LENGTH=2048U
CY_IGNORE+=$(SEARCH_SigV4-for-AWS-IoT-embedded-sdk)/test
CY_IGNORE+=$(SEARCH_SigV4-for-AWS-IoT-embedded-sdk)/tools
CY_IGNORE+=$(SEARCH_SigV4-for-AWS-IoT-embedded-sdk)/docs
CY_IGNORE+=$(SEARCH_SigV4-for-AWS-IoT-embedded-sdk)/CMakeLists.txt
CY_IGNORE+=$(SEARCH_SigV4-for-AWS-IoT-embedded-sdk)/sigv4FilePaths.cmake

# -----------------------------------------------------------------------------
# wslay (WebSocket frame codec used by KVS WSS signaling)
# Autoconf bypassed: auto-discovery builds lib/*.c, and we supply the
# config.h.in flags via DEFINES. HAVE_ARPA_INET_H makes wslay_net.h pull
# <arpa/inet.h>; lwIP's POSIX-compat path supplies that header (it forwards to
# lwIP's htonl/ntohl). WORDS_BIGENDIAN is left undefined -> Cortex-M is
# little-endian, so wslay's runtime byteswap path kicks in (correct).
# -----------------------------------------------------------------------------
LWIP_POSIX := ../../mtb_shared/lwip/STABLE-2_1_2_RELEASE/src/include/compat/posix
INCLUDES+=$(LWIP_POSIX)
DEFINES+=HAVE_ARPA_INET_H
# wslay.h includes <wslay/wslayver.h> which is generated by autoconf from
# wslayver.h.in. Define WSLAY_VERSION up-front to skip that include (documented
# escape hatch in wslay.h). Pin matches deps/wslay.mtb (release-1.1.1).
DEFINES+=WSLAY_VERSION=\"1.1.1\"

CY_IGNORE+=$(SEARCH_wslay)/examples
CY_IGNORE+=$(SEARCH_wslay)/tests
CY_IGNORE+=$(SEARCH_wslay)/doc
CY_IGNORE+=$(SEARCH_wslay)/m4
CY_IGNORE+=$(SEARCH_wslay)/cmake
CY_IGNORE+=$(SEARCH_wslay)/CMakeLists.txt
CY_IGNORE+=$(SEARCH_wslay)/lib/CMakeLists.txt
CY_IGNORE+=$(SEARCH_wslay)/lib/config.h.in
CY_IGNORE+=$(SEARCH_wslay)/configure.ac
CY_IGNORE+=$(SEARCH_wslay)/Makefile.am
CY_IGNORE+=$(SEARCH_wslay)/lib/Makefile.am

# -----------------------------------------------------------------------------
# libsrtp -- SRTP/SRTCP encryption for the media path.
# Backend: libsrtp's internal AES (OPENSSL/MBEDTLS/NSS/WOLFSSL all undefined,
# see webrtc/config.h). CM33 has CPU headroom for SW-AES at our framerate.
# Profile used: SRTP_AES128_CM_HMAC_SHA1_80 -> AES-GCM not needed.
#
# config.h: shared `webrtc/config.h` is project-owned; HAVE_CONFIG_H is defined
# below and ./webrtc is on the include path (above). webrtc/ also supplies our
# netinet/in.h shim (lwIP ships <arpa/inet.h> but not <netinet/in.h>, which
# libsrtp's datatypes.h needs for htonX/ntohX).
#
# Auto-discovery would build libsrtp's whole tree; we want only the internal
# AES + HMAC-SHA1 path. Ignore the host-only/build dirs and the alternate
# crypto backends (OpenSSL/MbedTLS/NSS/wolfSSL shims + AES-GCM) so what remains
# is exactly: srtp/srtp.c, crypto/cipher/{cipher,cipher_test_cases,aes,aes_icm,
# null_cipher}.c, crypto/hash/{auth,auth_test_cases,hmac,sha1,null_auth}.c,
# crypto/kernel/*.c, crypto/math/datatypes.c, crypto/replay/*.c.
# -----------------------------------------------------------------------------
DEFINES+=HAVE_CONFIG_H
# Demote `incompatible-pointer-types` from error to warning. libsrtp's srtp.c
# passes `(unsigned int *)` where the cipher API takes `uint32_t *`. Both are
# 32-bit on Cortex-M but the types differ (newlib defines uint32_t as
# `unsigned long`), and modern GCC errors on this by default. Same flag the
# N6 reference port uses.
CFLAGS+=-Wno-error=incompatible-pointer-types

# Host-only / build-infra dirs.
CY_IGNORE+=$(SEARCH_libsrtp)/cmake
CY_IGNORE+=$(SEARCH_libsrtp)/doc
CY_IGNORE+=$(SEARCH_libsrtp)/fuzzer
CY_IGNORE+=$(SEARCH_libsrtp)/test
CY_IGNORE+=$(SEARCH_libsrtp)/timing
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/test
# Alternate crypto backends we do not compile (internal AES only).
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/cipher/aes_gcm_mbedtls.c
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/cipher/aes_gcm_nss.c
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/cipher/aes_gcm_ossl.c
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/cipher/aes_gcm_wssl.c
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/cipher/aes_icm_mbedtls.c
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/cipher/aes_icm_nss.c
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/cipher/aes_icm_ossl.c
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/cipher/aes_icm_wssl.c
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/hash/hmac_mbedtls.c
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/hash/hmac_nss.c
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/hash/hmac_ossl.c
CY_IGNORE+=$(SEARCH_libsrtp)/crypto/hash/hmac_wssl.c
