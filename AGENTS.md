# AGENTS.md

## Project Context

LunarNX is a Nintendo Switch homebrew Xbox Remote Play client. The codebase is
early-stage and prioritizes getting a reliable end-to-end stream running on real
Switch hardware over reducing implementation complexity.

Key areas:

- `src/auth/`: Microsoft/Xbox authentication and token storage.
- `src/api/`: HTTP client and Xbox REST API calls.
- `src/app/`: Xbox session, SDP/ICE, WebRTC transport, and stream controller.
- `src/webrtc/`: wrapper around libpeer.
- `src/stream/`: media pipeline, H.264/Opus decode, render/audio stats.
- `src/ui/`: Borealis UI activities and stream overlays.
- `tools/mock_xbox/`: local mock Xbox WebRTC server for simulator testing.
- `tools/libpeer_legacy/`: tracked patch for the ignored local libpeer checkout.
- `docs/ryujinx_testing.md`: current Ryubing/Ryujinx test workflow.

## Hard Constraints

- Do not build Switch-targeted libraries on macOS. Use Docker/devkitA64 for all
  Switch libraries and Switch NRO builds.
- Prefer `devkitpro/devkita64:20251117` for current Switch builds unless the
  user asks for a different image.
- Keep dependency usage aligned with Moonlight-Switch or wiliwili when this repo
  uses the same dependency class. If a library is unavailable in the Docker
  image, build it inside Docker.
- The active WebRTC path is the legacy local `lib/libpeer` checkout. It is
  ignored by the main repo; do not try to commit it directly. Reproducible
  libpeer changes belong in `tools/libpeer_legacy/legacy-libpeer-switch.patch`
  and `tools/libpeer_legacy/README.md`.
- Do not switch to `WEBRTC_PROVIDER=upstream` unless the user explicitly asks.
- Do not remove the legacy libpeer packet-buffer fix. `CONFIG_MTU` is the RTP
  packetization size; the inbound UDP receive buffer is `CONFIG_PACKET_BUFFER_SIZE`.
- App startup should stay light. Network failures must show errors and must not
  prevent the app from opening.
- Keep Switch NRO BSS below 32 MiB. This is a LunarNX regression guard based on
  known-working LunarNX, Moonlight-Switch, and wiliwili packages, not a general
  Switch platform limit. Do not add large static buffers or static fallback
  heaps. Use libnx's default dynamic heap initialization, as Moonlight-Switch
  does, unless real-hardware evidence requires a different implementation.
- Do not eagerly construct or start streaming controllers before the user enters
  the relevant flow.
- Do not add Applet Mode blocking pages unless the user explicitly asks.
- Never commit tokens, auth files, simulator data dirs, generated logs, or
  generated NRO/build artifacts.

## Build Commands

Switch builds must use Docker:

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    set -e
    export DEVKITPRO=/opt/devkitpro
    export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
    make -f Makefile.switch -j$(nproc) \
      NETWORK_DIAG=0 CURL_PROVIDER=wiliwili CURL_VERIFY=0 \
      CURL_VERBOSE=0 CURL_TIMEOUT_MS=30000
  '
```

For a clean Switch rebuild, run `make -f Makefile.switch clean` inside the same
Docker environment before the build command.

Desktop builds/tests may use macOS system libraries because they are not Switch
artifacts:

```sh
make -f Makefile.desktop
make -f Makefile.desktop auth_test
```

## Test Commands

Run focused Python regressions after WebRTC, session, RTP, data channel, or
stream startup changes:

```sh
python3 tests/xbox_stream_session_order_test.py
python3 tests/libpeer_sctp_config_test.py
python3 tests/libpeer_dtls_read_loop_test.py
python3 tests/datachannel_ppid_test.py
python3 tests/switch_nro_bss_test.py
```

Run formatting whitespace checks before finishing:

```sh
git diff --check
```

For simulator testing, prefer Ryubing Canary 1.3.333 and follow
`docs/ryujinx_testing.md`. Avoid raw Ryubing stdout/stderr logs for long runs;
they can grow extremely large. Use the app log instead:

```sh
$HOME/work/self/ryujinx-data/sdcard/switch/LunarNX/lunarnx.log
```

Mock streaming is usually tested with:

```sh
python3 tools/mock_xbox/mock_xbox_server.py \
  --video /tmp/lunarnx_test_720p60.mp4 \
  --public-ip 192.168.1.100 \
  --http-port 8080
```

Then run `build/switch/LunarNX.nro` in Ryubing with guest internet enabled.

## Simulator And Hardware Expectations

- Ryubing/Ryujinx is a development regression tool, not final proof. Real Switch
  hardware remains the final compatibility target.
- Ryubing accepting an NRO does not prove that hbmenu on real hardware can load
  its static memory image. A generic software-close error with no app log can be
  a pre-`main()` loader failure; check NRO BSS before changing startup UI code.
- Ryubing can validate startup, UI flow, local mock WebRTC, RTP/SRTP behavior,
  and basic decode/render pipeline correctness.
- Treat public internet behavior in older Ryujinx headless as unreliable.
- When streaming visuals look corrupted, check `missing`, `h264_corrupt`,
  `srtp_fail`, `decode_errors`, and `rtp_queue_drop` before changing render code.

## Engineering Practices

- Keep changes scoped to the requested behavior.
- Follow existing Borealis/libnx/FFmpeg/libpeer patterns in nearby files.
- Prefer explicit logs through `lunar::diagnosticLog` over ad hoc stdout/stderr
  spam in Switch runtime paths.
- Avoid adding new dependencies unless they are necessary and the build path is
  clear for Switch Docker builds.
- Do not revert user changes or generated local dependency checkouts unless the
  user explicitly asks.
- If a change touches startup, auth, networking, WebRTC, media, or Switch build
  behavior, include verification notes in the final response.

## Done Criteria

Before reporting completion for code changes:

- Build the Switch NRO in Docker when Switch code or build files changed.
- Run `python3 tests/switch_nro_bss_test.py` after every full Switch build.
- Run the focused tests relevant to the changed area.
- Run `git diff --check`.
- If the change affects streaming, run at least one Ryubing mock-stream smoke
  test or clearly state why it was not run.
- Summarize residual risk, especially when results come only from simulator and
  not real Switch hardware.
