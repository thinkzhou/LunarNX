# WebRTC transport evaluation

This is an isolated desktop experiment. It does not change LunarNX's active
legacy libpeer provider. Switch libraries must still be built in Docker.

## Comparison contract

- Use the checked-out LunarNX legacy libpeer and the pinned local libdatachannel.
- Use the same libdatachannel sender in a **separate process** for both receivers.
- Send synthetic 1200-byte H.264 RTP packets over actual ICE/DTLS/SRTP, in 60 Hz
  bursts at identical requested rates. This tests transport, not video decoding.
- Measure receiver process CPU time, delivered packets, and sender-to-callback
  latency using a shared host monotonic clock. Exclude a warm-up interval.
- Alternate receiver order between repeats. Record versions, build options,
  requested/actual send rates, and failed connections.
- Do not compare data-channel throughput with RTP throughput. The first experiment
  disables data channels on both sides so SCTP implementation differences cannot
  masquerade as media transport differences.
- Local-host results do not prove Switch performance, WAN NAT success, Xbox
  interoperability, image quality, or end-to-end input/display latency.

Generated binaries, SDP, and result JSON belong under `build/webrtc_bench/`.
No credentials or external signaling services are needed.

## Run on macOS

Prerequisites: the existing LunarNX native mbedTLS/libSRTP archives under
`lib/libpeer/third_party/*/build_host`, CMake, a C++17 compiler, Homebrew OpenSSL,
and an initialized `github_repos/libdatachannel` checkout with submodules.
The recipe uses IPv4 on `en0`; it is not a general Linux or Switch runner.

```sh
bash tools/webrtc_bench/build_native.sh
python3 tools/webrtc_bench/run_trials.py --rates 15 30 50 --repeats 3 --seconds 6
```

Both receivers use the **same libSRTP static archive** (internal software
AES-CM/HMAC-SHA1, without GCM). libdatachannel uses OpenSSL for DTLS, while
libpeer uses its existing native mbedTLS. Consequently connection-time results
include TLS-backend differences. Neither is the donor project's exact Switch
crypto configuration. Bind both endpoints to the same IPv4 address using
`--bind-address`; the default is the current `en0` address.

`--idle-us 1000` gives the libpeer owner loop a one-millisecond idle sleep.
The libdatachannel transport has its own event-driven worker threads; its main
thread checks completion with the same sleep. These are explicit harness
scheduling choices, not measurements of the full LunarNX owner loop.

Each run stores logs, SDP and raw JSON under `build/webrtc_bench/trials-*`.
Handshake diagnostics are off in measured builds. Inspect `results.json` for
failed trials as well as successes. Packet timestamps are recorded immediately
before the common sender calls `Track::send`, so latency includes sender-side
queueing and both processes' OS scheduling. Packet payloads are synthetic and
must not be fed to a decoder. No RTP recovery, audio, SCTP, UI or decode runs.
