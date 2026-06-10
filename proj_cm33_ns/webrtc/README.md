# proj_cm33_ns/webrtc/ — WebRTC subsystem layout

## Layering rule (scope-relative)

When this doc says "top level," it means **top of the current scope**, not project root.

- **Project level:** `proj_cm33_ns/` is the project. Everything outside `webrtc/` (`app_task.c`, `wifi_app.c`, `main.c`, build files, etc.) is the project's application code. The project imports the WebRTC subsystem.
- **WebRTC subsystem level:** `webrtc/` is the WebRTC subsystem.
  - From the project's perspective, `webrtc/` is one of the modules the project depends on.
  - From third-party / upstream protocol code's perspective, `webrtc/` *is* the application — it's the consuming code that drives the protocol libraries.
- **WebRTC app code:** the files directly under `webrtc/` are our solution at the WebRTC-application layer. They orchestrate sessions, drive frames, and smoke-test the stack. They consume the three tiers underneath.
- **Tiers:** `algorithm/`, `util/`, `shim/` — building blocks consumed by the WebRTC-app code.

## Initial tree (canonical reference)

This is the structure as of the S1 restructure. Use this as the reference when discussing placement.

```
proj_cm33_ns/                       ← project root. THE app (project-level).
├── app_task.c                      ← project-level app code
├── wifi_app.c                      ← project-level app code, peer of app_task
├── main.c                          ← project entry
├── (project config: app_config.h, app_eeprom_data.c, FreeRTOSConfig.h, etc.)
└── webrtc/                         ← THE WebRTC app (webrtc-level).
    │                                  From upstream's perspective, this whole dir is "the app."
    │                                  From the project's perspective, this is the webrtc subsystem.
    │
    ├── README.md                   ← this file
    │
    ├── app_webrtc.{c,h}            ← webrtc-app top-level: session orchestrator (FreeRTOS task,
    │                                  AWS creds wire-in from IoTC, retry/backoff). Called from
    │                                  proj_cm33_ns/app_task.c.
    ├── aws_creds.h                 ← webrtc-app top-level: creds plumbing for the orchestrator
    ├── media_source_ring.{c,h}     ← webrtc-app top-level: M2 shmem ring → frame callbacks for
    │                                  the protocol stack. Drains H.264 NALs from CM55 encoder.
    ├── webrtc_smoke_test.{c,h}     ← webrtc-app top-level: smoke test that exercises the
    │                                  protocol libs (SigV4, STUN, H.264 packetize, SDP, libsrtp).
    │                                  Run once at startup before any real integration.
    │
    ├── algorithm/                  ← protocol-correctness code (the WebRTC RFCs)
    │   ├── signaling.{c,h}         ← KVS WSS + SigV4 + JSON envelope over secure-sockets TLS.
    │   │                              Hardware-verified. Drives wslay. Implements signaling
    │   │                              protocol using third-party libs (sigv4, wslay) as inputs.
    │   ├── peer_connection.{c,h}   ← peer-connection lifecycle + answer build. Ours; doomed —
    │   │                              answer builder is broken (template-based, doesn't parse
    │   │                              offer). Will be replaced by upstream awslabs reference.
    │   ├── ice_controller.{c,h}    ← ICE pair table + STUN/srflx + connectivity checks. Ours;
    │   │                              outbound side verified, inbound never exercised. Will be
    │   │                              replaced by upstream awslabs reference.
    │   └── dtls_transport.{c,h}    ← DTLS cert/key/fingerprint + UDP socket. Ours; handshake +
    │                                  verify + keying export not implemented (functions declared,
    │                                  bodies empty). Will be replaced by upstream's
    │                                  transport_dtls_mbedtls.c.
    │
    ├── util/                       ← small helpers (initially empty)
    │                                  Will be populated with upstream utilities as they are
    │                                  needed by algorithm/ files coming in: string_utils,
    │                                  timer_controller, message_queue, networking_utils.
    │                                  Each is a thin pure-C or FreeRTOS-wrapper utility.
    │
    └── shim/                       ← shims adapting third-party to our system
        ├── csprng.{c,h}            ← PSE84 hardware RNG → mbedTLS entropy callback. Provides
        │                              DRBG-grade randomness to mbedTLS-using algorithm code.
        │                              No upstream analog.
        ├── config.h                ← HAVE_CONFIG_H header consumed by libsrtp.
        ├── libsrtp_config/         ← libsrtp build-time config files.
        └── netinet/                ← vendor compat headers (BSD-style netinet/in.h polyfill).
```

## Tier definitions

### `algorithm/` — protocol code

Implements WebRTC protocol algorithms (SDP, ICE, DTLS, peer-connection lifecycle, SRTP, RTCP). Pure C, takes byte buffers in / byte buffers out, no platform binding worth tweaking. Comes from one of:

- **Upstream awslabs reference application** ([awslabs/freertos-webrtc-reference-on-amebapro-for-amazon-kinesis-video-streams](https://github.com/awslabs/freertos-webrtc-reference-on-amebapro-for-amazon-kinesis-video-streams), `examples/` subtree). Apache-2.0, Amazon copyright. Verified-working in two downstream projects (Ameba reference, STM32 N6 port).
- **Our own implementations** that exist as protocol code today but are slated for replacement by the upstream equivalents (`peer_connection`, `ice_controller`, `dtls_transport` per the initial tree above).
- **Our own implementations that stay** (`signaling.c` — hardware-verified, hooks into our secure-sockets TLS stack which upstream's signaling doesn't).

When a file in `algorithm/` is upstream-origin, the file keeps its upstream Apache-2.0 header and is treated as owned source we tailor freely (per GUIDELINES.md "Copy-from-upstream policy"). Restyling and per-project conventions wait for the post-end-to-end-green debt-rework pass.

### `util/` — small helpers

Thin utilities used by `algorithm/` files. FreeRTOS wrappers (queues, timers), pure-C string and parsing helpers. Each is small enough to read in a sitting. Coming from upstream's `examples/{string_utils, timer_controller, message_queue, networking_utils}/` as algorithm files that depend on them are pulled in.

### `shim/` — shims and glue

Adapters between third-party expectations and our system. Three flavors:

1. **System → mbedTLS** plumbing — e.g., `csprng.{c,h}` exposes PSE84 hardware RNG via an mbedTLS entropy callback.
2. **Upstream macros → our facilities** — e.g., a future `logging.h` shim mapping upstream's `LogError(())` / `LogInfo(())` family to our project logging path. A future `metric.h` stub will compile out upstream's telemetry calls to no-ops.
3. **Build-time configuration headers** for third-party libs — e.g., `config.h` (HAVE_CONFIG_H for libsrtp), `libsrtp_config/`, `netinet/in.h` polyfill.

Shim is **purpose-built to live**. It's not a holding pen for transient files. A file in `shim/` belongs there because its job is to translate, not because it's about to be replaced.

### WebRTC-app code (files directly under `webrtc/`)

Our code that drives the protocol stack and provides frames into it. From the WebRTC application's perspective, this is the "main" of the WebRTC subsystem.

- `app_webrtc.{c,h}` — the orchestrator. Owns the WebRTC FreeRTOS task. Wires AWS creds from IoTC discovery. Drives session lifecycle (wait for offer → set up peer connection → block on signaling → backoff retry). Called from `proj_cm33_ns/app_task.c`.
- `media_source_ring.{c,h}` — frame pipeline. Drains the M2 shmem ring (`shared/include/video_ring.h`) and feeds H.264 NALs into the protocol stack via the peer-connection's `WriteFrame` entry point. Will be renamed and rewritten in a later S-step to satisfy upstream's `AppMediaSource_Init` callback contract.
- `webrtc_smoke_test.{c,h}` — bring-up smoke test. Runs pure-computation checks against the third-party WebRTC libs (sign a fake KVS URL with SigV4, serialize+deserialize a STUN binding request, packetize one H.264 NAL, build a minimal SDP, init libsrtp). Called once at startup. Verifies link integrity and basic API paths before real integration code runs.
- `aws_creds.h` — type declarations for the AWS creds struct (region, channel ARN, AKID, secret, session token) the orchestrator uses.

## Where new code goes

When pulling in a file from upstream or writing new code, place it by *role*, not by *lifecycle*:

| Role | Goes in |
|---|---|
| Implements WebRTC protocol (SDP/ICE/DTLS/peer-connection/SRTP/RTCP correctness) | `algorithm/` |
| Small reusable helper consumed by algorithm code | `util/` |
| Adapter between third-party expectations and our system facilities | `shim/` |
| Our solution code that drives the protocol stack from our side | top-level (under `webrtc/` directly) |

A file's eventual fate (kept, replaced, deleted) does not change its current placement.

**Flat layout by default inside each tier.** Place files directly under `algorithm/`, `util/`, `shim/`. Only create a subdirectory when an upstream module is genuinely multi-file with its own namespace and dropping its files flat would lose clarity (e.g., a codec-helper module with several sibling `.c` files). The per-S-step lands the call.

## Provenance — what came from where

Per GUIDELINES.md "Copy-from-upstream policy," files that originated as upstream copies keep their upstream copyright header on initial copy and are tailored freely afterward.

All files inside each tier directory are flat — no subdirectories. Upstream multi-file modules (peer_connection's 17 files, codec helpers) all sit at the same level with their `peer_connection_*` / `sdp_controller_*` prefixes carrying the grouping.

| Current contents | Origin |
|---|---|
| `algorithm/signaling.{c,h}` | Ours (Avnet MIT). Kept — hooks into our secure-sockets TLS stack. |
| `algorithm/ice_controller.{c,h}` | Ours (Avnet MIT). Doomed — pending replacement by upstream `examples/ice_controller/`. |
| `algorithm/dtls_transport.{c,h}` | Ours (Avnet MIT). Doomed — pending replacement by upstream `examples/network_transport/transport_dtls_mbedtls.*` + UDP BIO + lwIP UDP wrapper. The cert/key/fingerprint knowledge here carries forward via `shim/csprng` feeding the upstream DRBG. |
| `algorithm/peer_connection.{c,h}` + `peer_connection_data_types.h` | Upstream awslabs `examples/peer_connection/peer_connection.{c,h}` + `peer_connection_data_types.h` (Apache-2.0). |
| `algorithm/peer_connection_sdp.{c,h}` | Upstream awslabs `examples/peer_connection/peer_connection_sdp.{c,h}` (Apache-2.0). The offer-driven answer builder. |
| `algorithm/peer_connection_srtp.{c,h}` + `peer_connection_srtcp.{c,h}` | Upstream awslabs `examples/peer_connection/` (Apache-2.0). SRTP/SRTCP framing over libsrtp. |
| `algorithm/peer_connection_jitter_buffer.{c,h}` + `peer_connection_rolling_buffer.{c,h}` | Upstream awslabs `examples/peer_connection/` (Apache-2.0). RX buffering — defer skip-decision to debt-rework. |
| `algorithm/peer_connection_codec_helper.h` + `peer_connection_{h264,h265,opus,g711}_helper.{c,h}` | Upstream awslabs `examples/peer_connection/peer_connection_codec_helper/` (Apache-2.0), flattened. Trim non-H.264 in debt-rework. |
| `algorithm/transceiver_data_types.h` | Upstream awslabs `examples/peer_connection/transceiver_data_types.h` (Apache-2.0). |
| `algorithm/sdp_controller.{c,h}` + `sdp_controller_data_types.h` | Upstream awslabs `examples/sdp_controller/` (Apache-2.0). |
| `util/string_utils.{c,h}` | Upstream awslabs `examples/string_utils/` (Apache-2.0). |
| `shim/csprng.{c,h}` | Ours (Avnet MIT). Kept — no upstream analog. |
| `shim/logging.h` | Ours (Avnet MIT). Maps upstream `LogError(())` family to our project logging. |
| `shim/config.h` | Ours. Project glue for libsrtp. |
| `shim/libsrtp_config/`, `shim/netinet/` | Vendor compat / build config. |
| `app_webrtc.{c,h}` | Ours (Avnet MIT). `app_webrtc.c` is currently an **S6a stub** that preserves the public surface and boots a no-op task; the real orchestration body lands in S6b on top of upstream `PeerConnection_*` APIs. `app_webrtc.h` public surface preserved (viewer→master comment wording corrected in S6a). |
| `app_webrtc.c.bak` | Ours (Avnet MIT). **Reference-only, not built.** Previous hand-rolled implementation kept under version control so S6b has the prior platform plumbing (creds populate, region parse, signaling/wslay/ICE wiring, retry/backoff) side-by-side with the upstream APIs being wired in. |
| `media_source_ring.{c,h}` | Ours (Avnet MIT). Body rewrites in a later S-step. |
| `webrtc_smoke_test.{c,h}` | Ours (Avnet MIT). |
| `aws_creds.h` | Ours (Avnet MIT). |
| Future `algorithm/ice_controller*.{c,h}` | Upstream awslabs `examples/ice_controller/` (Apache-2.0) |
| Future `algorithm/transport_dtls_mbedtls.{c,h}` + UDP BIO + UDP wrapper | Upstream awslabs `examples/network_transport/` (Apache-2.0) |
| Future `util/timer_controller.{c,h}` | Upstream awslabs `examples/timer_controller/` (Apache-2.0) |
| Future `util/message_queue.{c,h}` | Upstream awslabs `examples/message_queue/` (Apache-2.0) |
| Future `util/networking_utils.{c,h}` | Upstream awslabs `examples/networking/networking_utils/` (Apache-2.0) |
| Future `shim/metric.h` | Our shim stubbing upstream's telemetry calls to no-ops |
