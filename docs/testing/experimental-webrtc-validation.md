# Experimental WebRTC backend validation

2026-10-30, Apple M2 host. These are functional smoke tests, not Switch
performance measurements. The production default remains legacy libpeer.

## Native streaming

Both backends link into the same native app and use the real local Xbox mock
server (aiortc), the existing Xbox session/channel protocol, media pipeline,
SDL rendering/audio queue, and input encoder. Each run uses an isolated generated
directory with mock-only configuration.

| Backend | Rendered video frames | Audio frames | Input packets | Clean exit |
| --- | ---: | ---: | ---: | --- |
| libdatachannel | 506 | 646 | 1563 | yes |
| libpeer | 721 | 653 | 1599 | yes |
| libdatachannel, fresh reconnect | 778 | 650 | 1630 | yes |

Counts differ with startup, timing and host load; they are not an FPS or speed
comparison. Video counts are recorded after a successful renderer call.

A second probe reuses **one StreamController in one process**, changes the
persisted backend between sessions, and includes xCloud mock negotiation:

| Cycle | Backend / session | Video | Audio | Input | Stop time |
| --- | --- | ---: | ---: | ---: | ---: |
| 0 | libdatachannel / home | 298 | 249 | 628 | 100 ms |
| 1 | libpeer / home | 300 | 262 | 628 | 100 ms |
| 2 | libdatachannel / cloud | 298 | 248 | 628 | 111 ms |
| 3 | libdatachannel / home | 297 | 248 | 629 | 120 ms |

The mock records all four DCEP channels, message handshake/authorization, valid
sequenced gamepad packets, and deterministic rumble response. This verifies
protocol delivery and lifecycle, not physical controller vibration.

The fixture now installs its passive DTLS role **before** aiortc's
`setLocalDescription()`. Previously it rewrote only the returned SDP, allowing
actual and advertised roles to disagree. An async regression checks the
installed local description; the native runs exercise the actual DTLS handshake.

## Build and checks

- Pinned SDK builds on native macOS and inside `devkitpro/devkita64:20251117`.
- Full dual-backend Switch NRO builds in Docker; BSS check passes at 20.4 MiB.
- Backend configuration file tests pass with the feature enabled and omitted,
  including a saved unavailable selection, trailing garbage, embedded NUL, an
  exact-64-KiB valid file and a larger file with a valid 64-KiB prefix.
- Existing native outbound-queue and stream-resilience scenario tests pass with
  the experimental feature disabled.
- Mock protocol tests: 17 pass.
- Session-order, SCTP configuration, DTLS read-loop, PPID and Chiaki ABI checks
  pass. The SCTP source-contract test now accepts both the fixed and configurable
  bounded receive-loop forms; no legacy dependency source was changed.
- Artifact packaging smoke includes the new MPL/MIT/BSD texts, pinned source
  reconstruction record and exact application source revision; workflow checks pass.
- Python/shell syntax, locale JSON parsing and `git diff --check` pass.

Generated evidence is under `build/mock-runs/`, `build/libdatachannel-sdk/`, and
`build/switch-build-final.log`; credentials, logs, build output and simulator
state are not committed.

## Simulator and hardware limits

The available legacy Ryujinx Headless SDL2 loaded the new NRO into `main()`
and reached application/Chiaki initialization during a bounded 30-second run.
This is a loader/startup smoke only. Canary 1.3.333 was unavailable in this
environment; no full Ryubing mock-stream or manual settings interaction was
completed. Native full-pipeline tests above are separate evidence, not a
replacement claim for simulator streaming or hardware validation.

Real Switch Xbox home, WAN Remote Play and xCloud must still be tested. PS
continues through Chiaki and is not covered by this WebRTC backend experiment.
libdatachannel does not currently expose the same ICE RTT and SRTP failure
counters as the legacy adapter; unavailable counters must not be interpreted as
proof of a healthy network.
