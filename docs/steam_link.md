# Steam Link integration

The Switch build now contains a build-time selectable Steam Link entry point
(`STEAMLINK=1`, the default). The first vertical slice implements the part of
Steam Remote Play that can be tested independently of the media session:

- Steam host discovery through the Remote Play UDP broadcast protocol.
- Persistent client identity generation under `sdmc:/switch/LunarNX/`.
- Four-digit pairing-code authorization using [ihslib](https://github.com/mariotaku/ihslib).
- Switch UI for host selection, PIN entry, and authorization errors.

Build dependencies are pinned as Git submodules:

```sh
git submodule update --init --recursive
```

After pairing, enter the optional host security PIN and select Start Steam
stream. The adapter requests desktop streaming at up to 1280x720, negotiates
H.264/Opus, and feeds LunarNX's media pipeline. H.264 codec data supplied as
Annex B or AVCDecoderConfigurationRecord is normalized and included with frames.
Pair and start streaming in the same app run; account/device authorization
resumption across launches is not implemented yet.

The pairing direction is important: LunarNX generates and displays the
four-digit pairing code, and the user enters that code in Steam's `Pair Steam
Link` dialog on the host. The host's optional fixed Remote Play security code
is a separate setting, entered in the host security PIN field on LunarNX.

The bundled ihslib discovery, authorization and session sources are linked.
Input currently uses a 125 Hz keyboard fallback; native controller HID reports,
analog sticks and rumble are not implemented. The ihslib dependency is
LGPL-3.0; keep its license and notices when distributing a build.

## Diagnostics and simulated verification

Build Switch artifacts only in Docker with STEAMLINK=1 APP_DIAG=1 DROP_DIAG=1.
Read `sdmc:/switch/LunarNX/lunarnx.log` after a failed test. Phase tags include
steam-link, steam-session, steam-video, steam-audio and steam-input. Protocol
Debug/Verbose output is discarded; Warning/Info forwarding is limited to one
message per level per second. Error messages and explicit phase logs remain.

Run `python3 tests/steam_link_runtime_sim_test.py` on a desktop with clang++ and
FFmpeg/libx264. It exercises the production cancellation, input pump, key edge
retry, session cleanup and codec-data helpers under ASan/UBSan. A generated
H.264 stream has SPS/PPS separated from its frames, is reconstructed using the
production helper, and decoded to matching frame hashes. This does not emulate
Steam authentication, libnx input, NVDEC, audio output or a real Steam session.
The Xbox Ryubing mock cannot validate the Steam wire protocol. Real Switch
Steam streaming and a Ryubing smoke test have not been run for these fixes.

## Desktop discovery probe

The dependency-free probe can validate the network discovery path without a
Switch NRO:

```sh
python3 tools/steamlink_probe.py --target 255.255.255.255 --timeout 6
```

Use `--target <host-ip>` to avoid broadcast while testing a known host. The
probe parses the same discovery framing and status fields used by LunarNX.

For authorization, `tools/steamlink_probe/authorize.c` is a small POSIX
ihslib client. It must be compiled with the vendored ihslib sources and a
desktop mbedTLS build, then run with the four-digit pairing code as its only
argument. Authorization should be performed only against a Steam host you
control; the code is passed on the local command line and is not stored.

## Desktop stream probe

The desktop probe links the same ihslib session sources used by the eventual
Switch adapter. It requests the host desktop, negotiates H.264 plus Opus, saves
the received elementary streams, and opens H.264/HEVC video in `ffplay` when it
is installed:

```sh
tools/steamlink_probe/build_desktop_stream_probe.sh
build/steamlink-desktop-stream-probe --pin <remote-play-security-pin> --duration 60
```

`--pin` is the host's Remote Play security PIN and is separate from the
four-digit pairing code. If the device was paired by an earlier process, use
`--pair-code <four-digit-pairing-code>` to authorize and stream in one process;
this is useful for testing clients that do not yet persist Steam's device token:

```sh
build/steamlink-desktop-stream-probe --pair-code <pairing-code> \
  --pin <remote-play-security-pin> --duration 60
```

`--no-display` keeps the probe headless while still recording and counting media
callbacks. A successful end-to-end run must show
`streaming request success`, `session connected`, a negotiated `video start`,
and non-zero video/audio packet summaries. A host with no running game may
reject the request or require a security PIN; that is a host-state result, not
evidence that the session transport works.
