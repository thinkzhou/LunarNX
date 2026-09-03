# LunarNX Development Guide

This document contains build, validation, architecture, and simulator information for contributors. The project README is intentionally focused on installation and use.

## Development Priorities

- Real Nintendo Switch hardware is the compatibility target; simulator results are supporting evidence only.
- Build Switch-targeted code and dependencies inside the pinned devkitA64 Docker image, not with macOS system toolchains.
- Keep the active WebRTC provider on the tracked legacy libpeer path unless a change explicitly targets another provider.
- Keep startup light and keep the Switch NRO BSS below 32 MiB.
- Never commit tokens, authentication files, logs, simulator data, or generated build artifacts.

## Prerequisites

- Git
- Docker
- macOS or Linux as the host operating system
- The `devkitpro/devkita64:20251117` Docker image

## Prepare Dependencies

```sh
git clone https://github.com/thinkzhou/LunarNX.git
cd LunarNX
./scripts/setup_dependencies.sh
```

The setup script fetches the pinned Borealis and legacy libpeer revisions, then applies the tracked Borealis patch, the ordered legacy libpeer patch series, and the nested libsrtp, Mbed TLS, and usrsctp Switch compatibility patches. The local dependency checkouts under `lib/` are intentionally ignored by the main repository; reproducible changes belong in the tracked patch files under `tools/`.

## Build The Switch NRO

The full build wrapper rebuilds Switch dependencies and LunarNX inside Docker:

```sh
./scripts/docker_build_full.sh
```

The equivalent application-only build is:

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    set -e
    export DEVKITPRO=/opt/devkitpro
    export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
    make -f Makefile.switch clean
    make -f Makefile.switch -j$(nproc) \
      NETWORK_DIAG=0 CURL_PROVIDER=moonlight CURL_VERIFY=0 \
      CURL_VERBOSE=0 CURL_TIMEOUT_MS=30000
  '
```

The resulting application is written to `build/switch/LunarNX.nro`.

The combined Xbox/PlayStation build must use `CURL_PROVIDER=moonlight`. That provider supplies the curl 8.x WebSocket support required by PSN signaling; the devkitPro/wiliwili curl 7.69 path cannot open the required `wss://` connection. The default release build uses `IPV6=0`. Pass `IPV6=1` when testing native IPv6 paths. This does not add TURN relay support.

## Desktop Development

The desktop target supports protocol probes and regression tests. It is not a replacement for a Switch build or real-hardware validation.

```sh
make -f Makefile.desktop
make -f Makefile.desktop auth_test
make -f Makefile.desktop stream_tests
make -f Makefile.desktop xcloud_session_support_test
```

Additional diagnostic targets include `sdp_probe`, `xcloud_handshake_probe`, and `xcloud_stream`.

## Validation

Run the focused tests for the changed area. WebRTC, session, RTP, data-channel, or stream-startup changes require at least:

```sh
scripts/check_stream_regressions.sh
python3 tests/xbox_stream_session_order_test.py
python3 tests/libpeer_sctp_config_test.py
python3 tests/libpeer_dtls_read_loop_test.py
python3 tests/datachannel_ppid_test.py
python3 tests/switch_nro_bss_test.py
git diff --check
```

Switch source or build changes require a clean Docker build before completion. Streaming changes should receive a Ryubing mock-stream smoke test and, whenever possible, real-hardware testing.

If GLSL shaders change, rebuild the generated `.dksh` files inside a devkitPro environment:

```sh
./scripts/compile_shaders.sh
```

## Simulator Testing

Use Ryubing Canary 1.3.333 and follow [ryujinx_testing.md](ryujinx_testing.md). Prefer the application log at:

```text
$HOME/work/self/ryujinx-data/sdcard/switch/LunarNX/lunarnx.log
```

Avoid collecting long raw emulator stdout/stderr logs because they can grow very large. The simulator can validate startup, UI flow, local mock WebRTC, RTP/SRTP behavior, and the basic media pipeline, but real Switch hardware remains the final compatibility target.

## Architecture

```text
                         Borealis UI
               platform, library, console lists
                              |
               +--------------+--------------+
               |                             |
        Xbox application path         PlayStation path
    Auth + GSSV/REST Sessions       Discovery + PSN + Chiaki
    legacy libpeer WebRTC           local/remote connection plan
    ICE/DTLS-SRTP/DataChannel       Takion media/control transport
               |                             |
               +--------------+--------------+
                              |
                    shared MediaPipeline
             H.264/HEVC + Opus + NVDEC/Audren
                              |
                    deko3d presentation
```

| Path | Responsibility |
| --- | --- |
| `src/auth/` | Microsoft/Xbox authentication and token storage |
| `src/api/` | HTTP, console discovery, cloud catalog, and Xbox session APIs |
| `src/app/` | Stream profiles, session lifecycle, SDP/ICE, channels, and controller orchestration |
| `src/webrtc/` | LunarNX wrapper around the active legacy libpeer checkout |
| `src/ps/` | PlayStation discovery, pairing, PSN, Chiaki sessions, input, and haptics |
| `src/stream/` | H.264/HEVC/Opus decoding, rendering, audio, synchronization, and statistics |
| `src/input/` | Switch controller input, Xbox packet encoding, and rumble |
| `src/ui/` | Borealis activities, settings, lists, stream view, and overlays |
| `tools/mock_xbox/` | Local mock Xbox WebRTC server used for simulator tests |

## Technical References

- [Ryubing/Ryujinx workflow](ryujinx_testing.md)
- [WebRTC streaming latency audit](webrtc-streaming-latency-audit-2026-07-26.md)
- [Remote streaming ICE investigation](remote-streaming-ice-investigation-2026-07-13.md)
- [Xbox + PlayStation architecture](xbox_playstation_dual_streaming_design.md)
- [Legacy libpeer patch notes](../tools/libpeer_legacy/README.md)
- [Dependency patch maintenance backlog](dependency-patch-maintenance-backlog.md)
- [Technical plan](TECHNICAL_PLAN.md)

Read [CONTRIBUTING.md](../CONTRIBUTING.md) before opening a pull request and [SECURITY.md](../SECURITY.md) before sharing logs or reporting a vulnerability.
