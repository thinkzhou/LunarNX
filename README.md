# LunarNX

English | [简体中文](README.zh-CN.md)

An unofficial Xbox Remote Play and Xbox Cloud Gaming client for Nintendo Switch homebrew.

LunarNX implements the Xbox streaming client path from Microsoft authentication and Xbox session APIs through WebRTC transport, H.264/Opus decoding, Switch rendering, audio output, controller input, and rumble.

> [!WARNING]
> LunarNX is early-stage software intended for development and testing on real Nintendo Switch hardware. It requires a Switch homebrew environment, is not compatible with an unmodified retail console, and may fail or behave differently across networks, accounts, games, firmware versions, and Xbox service changes.

## Project status

Real Switch hardware is the compatibility target. Ryubing/Ryujinx is useful for regression testing, but simulator success is not proof of hardware compatibility.

| Area | Current status |
| --- | --- |
| Xbox console Remote Play on the same LAN | Primary tested path |
| Xbox console Remote Play over the internet | Experimental; requires direct UDP/NAT connectivity |
| Xbox Cloud Gaming (xCloud) | Experimental |
| 720p / 1080p / 1080p HQ | Available; stability and delivered bitrate depend on the network and server |
| H.264 hardware decoding | Available through Switch NVDEC |
| Native IPv6 | Optional build setting; disabled in the default Switch build |
| TURN relay | Not implemented |

Remote streaming is not guaranteed to work on every network. LunarNX can use public IPv4, native IPv6 when enabled, and IPv4 endpoints derived from Xbox Teredo ICE candidates, but the active legacy WebRTC stack does not provide a complete TURN relay fallback.

## Highlights

- Microsoft device-code sign-in with local token storage
- Discovery and wake-up of registered Xbox consoles
- Browsable and searchable xCloud library with recent and new-title sections
- Xbox GSSV session creation, SDP/ICE exchange, stale-session cleanup, keepalive, and reconnect handling
- H.264 hardware decoding using Tegra X1 NVDEC
- Zero-copy deko3d rendering, with copy-out and software decoder fallbacks
- Optional EASU upscaling, RCAS sharpening, and dithering
- Opus audio decoding with low-latency libnx Audren output
- Switch controller to Xbox input mapping and four-motor rumble support
- 720p at 10 Mbps, 1080p at 20 Mbps, and 1080p HQ at 30 Mbps receiver profiles
- Runtime performance overlay for bitrate, packet loss, decode, render, audio, and WebRTC statistics
- English, Simplified Chinese, and Traditional Chinese UI translations

## Screenshots

### 1080p streaming on Nintendo Switch

![LunarNX streaming an Xbox game at 1080p on Nintendo Switch](docs/screenshots/streaming.jpg)

| Console discovery | Stream settings |
| --- | --- |
| ![LunarNX Xbox console discovery screen](docs/screenshots/find_xbox.jpg) | ![LunarNX resolution and decoder settings](docs/screenshots/settings.jpg) |

![LunarNX establishing an Xbox Remote Play session](docs/screenshots/connecting.jpg)

## Requirements

- A Nintendo Switch capable of running homebrew NRO applications, typically with Atmosphère CFW
- Full-memory/title-override mode is strongly recommended; Applet Mode may not provide enough memory for streaming and hardware decoding
- An Xbox One or Xbox Series console with Remote Play enabled, or an account eligible for Xbox Cloud Gaming
- A stable 5 GHz Wi-Fi or wired network connection
- A legally obtained copy of all required firmware and system files for any simulator testing

Xbox Cloud Gaming availability depends on account entitlement and region. Game Pass Ultimate is commonly required, although eligible free-to-play titles may differ.

## Installation

1. Download `LunarNX.nro` from the [Releases page](https://github.com/thinkzhou/LunarNX/releases).
2. Copy it to your SD card:

   ```text
   sdmc:/switch/LunarNX/LunarNX.nro
   ```

3. Start the Homebrew Menu in title-override/full-memory mode and launch LunarNX.

Do not publish or share the contents of `sdmc:/switch/LunarNX/`. It can contain Microsoft/Xbox authentication material, cached account data, and diagnostic logs.

## First run

1. Select **Start sign-in**.
2. Open the displayed Microsoft device-login URL on a phone or computer and enter the code.
3. After authentication, choose either your registered Xbox console or the xCloud library.
4. Open Settings and select 720p, 1080p, or 1080p HQ.
5. Select **Play** or **Wake & connect**.

The first session may take up to a minute. A sleeping home console may require several wake-up attempts.

## Controls

LunarNX maps buttons by their physical position, so the Nintendo face-button labels are translated to the equivalent Xbox layout.

| Nintendo Switch | Xbox action |
| --- | --- |
| B (bottom) | A |
| A (right) | B |
| Y (left) | X |
| X (top) | Y |
| L / R | LB / RB |
| ZL / ZR | LT / RT |
| Minus | View |
| Plus | Menu |
| L + R + Plus (together) | Xbox Guide / Nexus button |
| Left stick click | L3 |
| Right stick click | R3 |
| D-pad / sticks | D-pad / sticks |

ZL and ZR are digital Switch buttons, so Xbox trigger input is reported as either 0% or 100%.

While streaming, press **L + R + Plus together** to open the Xbox Guide. LunarNX sends this chord as a single Xbox Guide / Nexus input, rather than forwarding LB, RB, and Menu separately.

To stop streaming, press **Minus + Plus together twice within three seconds**. A single Minus or Plus press remains available as Xbox View or Menu input.

## Building from source

### Prerequisites

- Git
- Docker
- The pinned `devkitpro/devkita64:20251117` image
- macOS or Linux as the host operating system

Do not build Switch-targeted libraries directly on macOS. Switch libraries and the NRO must be built with devkitA64 inside Docker.

### Prepare dependencies

```sh
git clone https://github.com/thinkzhou/LunarNX.git
cd LunarNX
./scripts/setup_dependencies.sh
```

The setup script fetches pinned Borealis and legacy libpeer revisions and applies the tracked LunarNX libpeer patch. These local dependency checkouts are intentionally ignored by the main repository.

### Build the Switch NRO

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    set -e
    export DEVKITPRO=/opt/devkitpro
    export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
    make -f Makefile.switch clean
    make -f Makefile.switch -j$(nproc) \
      NETWORK_DIAG=0 CURL_PROVIDER=wiliwili CURL_VERIFY=0 \
      CURL_VERBOSE=0 CURL_TIMEOUT_MS=30000
  '
```

The resulting application is written to:

```text
build/switch/LunarNX.nro
```

The default Switch build uses `IPV6=0`. Pass `IPV6=1` to the make command to compile native IPv6 support. This does not add TURN relay support and does not guarantee connectivity on IPv6-only or restrictive NAT networks.

### Desktop development build

The desktop target is intended for development, protocol probes, and regression tests. It is not a replacement for Switch hardware testing.

```sh
make -f Makefile.desktop -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
make -f Makefile.desktop stream_tests
make -f Makefile.desktop xcloud_session_support_test
```

Useful diagnostic targets include:

```sh
make -f Makefile.desktop auth_test
make -f Makefile.desktop sdp_probe
make -f Makefile.desktop xcloud_handshake_probe
```

### Validation

After Switch or streaming changes, run the relevant focused tests and at minimum:

```sh
scripts/check_stream_regressions.sh
python3 tests/xbox_stream_session_order_test.py
python3 tests/libpeer_sctp_config_test.py
python3 tests/libpeer_dtls_read_loop_test.py
python3 tests/datachannel_ppid_test.py
python3 tests/switch_nro_bss_test.py
git diff --check
```

The Switch NRO BSS regression guard is 32 MiB. Real hardware remains the final validation target. See [docs/ryujinx_testing.md](docs/ryujinx_testing.md) for the current simulator and mock-stream workflow.

If GLSL shaders change, rebuild the `.dksh` files inside a devkitPro environment:

```sh
./scripts/compile_shaders.sh
```

## Architecture

```text
Microsoft device-code authentication
                │
                ▼
      Xbox REST / GSSV APIs
  console discovery, catalog, sessions
                │
                ▼
       WebRTC via legacy libpeer
 SDP + ICE + DTLS-SRTP + SCTP channels
          │                 │
          ▼                 ▼
 H.264 / Opus media    Xbox input/control
          │                 │
          ▼                 ▼
 NVDEC + deko3d       Joy-Con / Pro Controller
 Audren audio         XInput + rumble
```

Major source areas:

| Path | Responsibility |
| --- | --- |
| `src/auth/` | Microsoft/Xbox authentication and token storage |
| `src/api/` | HTTP client, console discovery, cloud catalog, and Xbox session APIs |
| `src/app/` | Stream profiles, session lifecycle, SDP/ICE, channels, and controller orchestration |
| `src/webrtc/` | LunarNX wrapper around the active legacy libpeer checkout |
| `src/stream/` | H.264/Opus decoding, rendering, audio, synchronization, and statistics |
| `src/input/` | Switch gamepad reading, Xbox packet encoding, and rumble |
| `src/ui/` | Borealis activities, settings, lists, stream view, and overlays |
| `tools/mock_xbox/` | Local mock Xbox WebRTC server for simulator testing |

## Privacy and security

- Authentication tokens are stored locally on the SD card.
- Never upload token files, full diagnostic logs, raw Xbox API responses, console identifiers, or public IP addresses.
- Release builds keep application diagnostics, network diagnostics, and raw Xbox response tracing disabled.
- Debug builds can expose sensitive account and network metadata.

Please read [SECURITY.md](SECURITY.md) before reporting a vulnerability or sharing logs.

## Contributing

Contributions and hardware test reports are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request.

Useful reports include the selected stream profile, Switch model and firmware, network topology, whether the test was LAN or WAN, and a minimal redacted log excerpt. Do not attach complete logs.

## Credits

LunarNX builds on or learns from several open-source projects, including:

- [libpeer](https://github.com/sepfy/libpeer)
- [Borealis](https://github.com/XITRIX/borealis)
- [FFmpeg](https://github.com/FFmpeg/FFmpeg), [wiliwili](https://github.com/xfangfang/wiliwili), and Switch NVDEC work
- [libnx](https://github.com/switchbrew/libnx) and [deko3d](https://github.com/devkitPro/deko3d)
- [XStreaming](https://github.com/Geocld/XStreaming)
- [Greenlight](https://github.com/unknownskl/greenlight)
- [Moonlight-Switch](https://github.com/XITRIX/Moonlight-Switch)
- [libnxbox](https://github.com/ursusworks/libnxbox)
- [xbox-xcloud-player](https://github.com/unknownskl/xbox-xcloud-player)

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for dependency licenses and binary distribution obligations.

## License

LunarNX-owned source code is licensed under the [MIT License](LICENSE). Third-party source, libraries, patches, and linked release artifacts remain subject to their own licenses.

The current Switch FFmpeg build enables GPL components. Anyone distributing a linked NRO must review and satisfy the license obligations for the exact dependency build used. `THIRD_PARTY_NOTICES.md` is an engineering inventory, not legal advice.

LunarNX is not affiliated with, authorized by, or endorsed by Microsoft, Xbox, Nintendo, or the maintainers of the referenced projects. All product names and trademarks belong to their respective owners.
