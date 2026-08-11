# PlayStation media replay

This target tests the production path after chiaki has decrypted, reordered,
and FEC-recovered Remote Play media. It deliberately does not emulate Takion
UDP encryption or PSN signaling.

Generate a deterministic fixture and copy it to the emulator SD card:

```sh
tools/ps_media_replay/generate_fixture.sh /tmp/ps_media_replay.mp4
cp /tmp/ps_media_replay.mp4 \
  "$HOME/work/self/ryujinx-data/sdcard/switch/LunarNX/ps_media_replay.mp4"
```

To replay the exact H.264 video track previously used by the Xbox/Rho test,
preserving its encoded video bytes while adapting only the audio for Chiaki:

```sh
PS_MEDIA_REPLAY_VIDEO_SOURCE="$HOME/work/self/ryujinx-data/sdcard/test_stream.mp4" \
PS_MEDIA_REPLAY_DURATION=20 \
  tools/ps_media_replay/generate_fixture.sh \
  "$HOME/work/self/ryujinx-data/sdcard/switch/LunarNX/ps_media_replay.mp4"
```

Build the standalone NRO in Docker:

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    export DEVKITPRO=/opt/devkitpro
    export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
    make -f Makefile.switch.psmedia -j$(nproc)
  '
```

Run `build/switch-psmedia-software/LunarNXPsMediaReplay.nro`. The player converts MP4
H.264 packets to Annex-B access units, sends the Opus stream header and encoded
packets through `PsMediaBridge`, then uses the normal `MediaPipeline`, renderer,
and Audren implementations. The default software backend establishes a simulator
baseline without invoking Ryubing's NVDEC emulation. Use
`PSMEDIA_BACKEND=copyout` or `PSMEDIA_BACKEND=zerocopy` to test the production
hardware decoder path; each backend has an isolated build directory. Results are written to
`sdmc:/switch/LunarNX/ps_media_replay.log`.

PLC, one reported video loss, and a complete pipeline restart are injected
automatically at 2, 3, and 5 seconds. The 12-second result gate only passes if
decoded video, decoded audio, and all three fault paths completed without a
video decode error. Copy-out/software builds display the production RGBA frame
sink through NanoVG; zero-copy builds use the production Deko3D presentation
path. A successful frame counter alone is not proof of visible output, so
simulator runs should also verify that the known-good color test image is
visible before and after the automatic restart. Controls can also inject extra
events manually:

- `A`: inject one Opus PLC frame (`nullptr, 0`).
- `X`: report one lost video frame on the next sample.
- `Y`: stop and reinitialize the complete media pipeline.
- `B`: exit.

The fixture is generated and remains untracked. A future capture from the macOS
native probe can use the same callback-level replay boundary.

## Formal controller lifecycle probe

`Makefile.switch.psmock` builds the normal LunarNX application with a
development-only replay transport and an automatic lifecycle activity. This
exercises the formal path from `PsStreamController` through `PsMediaBridge`, the
production `MediaPipeline`, NVDEC/Deko3D, and Audren. It deliberately bypasses
PSN discovery, hole punching, signaling, and Takion because no live console is
involved.

The mock route is selected only when both safeguards are present:

- the NRO was compiled with `PS_MOCK_REPLAY=1`;
- `config.json` contains `"ps_network_profile": "mock_replay"`.

Normal builds compile with `PS_MOCK_REPLAY=0`, so a configuration file alone
cannot enable this route on a release or real-hardware build.

Build the automatic probe in Docker:

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    set -e
    export DEVKITPRO=/opt/devkitpro
    export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
    make -f Makefile.switch.psmock -j$(nproc) \
      NETWORK_DIAG=0 CURL_PROVIDER=moonlight CURL_VERIFY=0 \
      CURL_VERBOSE=0 CURL_TIMEOUT_MS=30000
  '
```

Copy `mock_config.json` and `ps_media_replay.mp4` into the simulator's
`sdcard/switch/LunarNX` directory, then run
`build/switch-psmock/LunarNX.nro`. No input is required. The activity creates a
controller, waits for decoded video and audio, stops and destroys it, and then
repeats the complete cycle. The result is written to
`sdmc:/switch/LunarNX/ps_mock_lifecycle.log`; success ends with:

```text
status=PASS rounds=2 reconnect=ok
```

This proves the controller-to-media lifecycle against deterministic callback
data. It does not prove PSN, NAT traversal, encrypted transport, or live-console
compatibility; those still require the native probe and final Switch hardware.
