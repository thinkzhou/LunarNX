# Xbox WebRTC backend A/B testing

Experimental builds contain **both** legacy libpeer and libdatachannel/libjuice.
The default stays **libpeer**. This setting affects Xbox Remote Play and xCloud
only. PS4/PS5 continue using Chiaki; their protocol is not WebRTC and cannot be
switched to libdatachannel by this setting.

## Switch between backends on the same Switch

1. Install the NRO from this PR's Switch build artifact, keeping your existing
   SD card configuration and credentials. Use the same NRO for both tests.
2. Disconnect the current stream, open **Xbox Settings → Connection backend →
   WebRTC backend**, and choose `libpeer (default)` or
   `libdatachannel (experimental)`.
3. Close settings to save, then connect to the same Xbox or cloud title. The
   choice is read when a new connection is created; an existing stream never
   changes transport underneath the media pipeline. No application restart is
   required.
4. Compare under the same network, title, scene, resolution, bitrate and video
   decoder. Test startup, audio, controls/rumble, stop, reconnect and foreground
   resume, as well as a longer session. Repeat with libpeer after the experiment.
5. If the experimental connection fails, return to Xbox Settings and choose
   libpeer. There is no silent automatic fallback that could invalidate an A/B
   result.

The persistent log contains `selected backend=libpeer` or
`selected backend=libdatachannel` for each new connection. This is present even
when verbose `APP_DIAG=0`. Avoid sharing credentials or full network traces.

For SD card configuration editing, the equivalent setting in
`sdmc:/switch/LunarNX/config.json` is:

```json
{
  "xbox_webrtc_backend": "libdatachannel"
}
```

Use `"libpeer"` to revert. Preserve the other keys in the existing file. Missing,
unknown, non-string or malformed configuration retains the libpeer default.

## What is shared, and what differs

Both backends use LunarNX's existing H.264 jitter/recovery policy, RTP clock
mapping, Opus/media pipeline, input sequencing and prioritized outbound queues.
The experimental adapter uses real libdatachannel ICE/DTLS/SRTP/SCTP and queues
its asynchronous callbacks onto the existing stream owner thread. Its incoming
media queue is bounded at 4 MiB / 4096 packets and preserves the startup gate.
It uses the same four Xbox DCEP channel labels, protocols and even stream IDs.

Switch builds reuse the existing mbedTLS, libSRTP and usrsctp libraries rather
than linking duplicate crypto stacks. The additional dependencies and their
Horizon adaptations are revision/hash pinned; see
[SDK preparation](../tools/libdatachannel_switch/README.md).

The experimental adapter currently lacks the legacy backend's ICE RTT and
SRTP failure-count instrumentation. A displayed zero for an unavailable metric
is **not** evidence of lower latency or zero decryption failures. RTT-dependent
policy uses its existing fallback when no valid ICE RTT sample exists.
Remote ICE behavior also differs; native IPv6 and strict-NAT/WAN success have
not been established for this Switch backend. Audio/render delay and physical
controller-to-display latency must be measured on hardware.

## Build and local verification

The standard Docker Switch build includes both implementations:

```sh
make -f Makefile.switch CURL_PROVIDER=moonlight EXPERIMENTAL_WEBRTC=1
```

Run that command inside the project's prescribed devkitA64 Docker environment.
`EXPERIMENTAL_WEBRTC=0` omits the experimental SDK and selector; clean the app
build when changing build feature flags. `WEBRTC_PROVIDER` stays `legacy`.
Native Make builds default to the feature being disabled; enable it explicitly
for local development.

Native full-pipeline verification against a local mock:

```sh
make -f Makefile.desktop EXPERIMENTAL_WEBRTC=1 webrtc_backend_mock_probe
# Start mock_xbox_server.py separately with a video containing video and audio.
build/pc/webrtc_backend_lifecycle_probe http://127.0.0.1:18080 build/new-webrtc-lifecycle-run
```

The output directory must not already exist. The probe runs home/cloud sessions
and backend changes in one process, asserts video/audio/input progress, and
checks bounded stop time. It uses only generated mock configuration.

See [recorded validation and limitations](testing/experimental-webrtc-validation.md).
Real Switch compatibility remains to be verified; the desktop and simulator
checks do not establish a performance improvement on hardware.
