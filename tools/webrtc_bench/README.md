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
