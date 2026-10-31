# Steam media loss recovery

The real Switch log showed 572/585 received and decoded frames followed by
video starvation while HID reports continued. One run timed out cleanly; the
other reported a sequence gap and media-window overflow before application exit.
There is no crash stack, so the exit's direct cause is not established.

Two receive-path defects were reproduced:

- Media expiry ran before the worker's wait loop, but never after subsequent
  condition-variable wakeups. A missing middle fragment therefore prevented
  delivery of complete newer frames until the receive ring filled.
- A forward packet-ID gap larger than the ring capacity was silently ignored
  without moving the old tail. All subsequent packets were ignored too, until
  the 16-bit sequence number wrapped.

The worker now expires stale assembly state on every poll/wakeup. Media windows
resynchronize after gaps larger than capacity. On overflow, the media ring is
fully reset and the arriving packet is retained (it may be the IDR head), while
the existing loss notification requests recovery. Reliable control windows keep
their ordered-delivery behavior.

Native replay covers 200 cycles of missing middle fragments, large sequence
gaps, late/out-of-order fragments and packet-ID wrap. It demonstrates that the
old Poll-only waiting behavior stays blocked while expiry-aware polling recovers.

Real Mac Steam A/B test, 2026-10-31: the desktop-only UDP wrapper drops media
datagrams 600 through 3599, then resumes normal delivery. Control traffic is not
dropped. With the pre-fix media sources, video stopped at 114 frames and remained
at 0 FPS after delivery resumed. With the fixes, a new keyframe arrived and video
returned to about 60 FPS: 1,174 frames / 2,228,889 bytes in 25 seconds, normal exit.

Reproduce the fixed run using an already saved host/pairing:

    sh tools/steamlink_probe/build_desktop_stream_probe.sh
    LUNAR_DROP_MEDIA_BURST=1 build/steamlink-desktop-stream-probe --duration 25 --no-display

Fault injection is desktop-only and opt-in; its source is not linked into Switch.
The build remains video/input only. No Ryubing smoke was run because the emulator
is unavailable; native protocol replay and real Steam streaming were used.

Steam already uses the shared PerfStats/StreamOverlay/PerfOverlay path used by
Xbox and PS. The detailed panel now shows Steam access units, unique submitted
frames, encoded bytes, codec, resolution, decoder errors and decode/render costs.
Xbox RTP/SRTP/loss rows are omitted for Steam because these measurements are not
available from IHS; zeros must not be mistaken for measured zero packet loss.

## Extended real-Steam validation

On 2026-10-31, an uninterrupted 180-second desktop session received 10,720 frames
and 22,628,141 bytes, exited normally, and all 10,720 captured H.264 frames decoded
successfully using PyAV/FFmpeg. The host may change capture dimensions to match
its window aspect ratio; the first offline-decoded frame was 1152x720.

A subsequent 300-second session enabled both `LUNAR_DROP_MEDIA_BURST=1` and
`LUNAR_REPEAT_MEDIA_LOSS=1`: the original 3,000-datagram loss plus nine 500ms
outages at 30-second intervals. Each outage recovered a keyframe. It received
17,372 frames / 25,324,805 bytes; all 17,372 captured frames decoded successfully.
No window overflow/permanent freeze was logged and the process exited normally.
A fresh process then reused the saved IP/key and received 216 frames in 5 seconds.
RSS was sampled every 15 seconds: roughly 2.8-4.1 MiB, with decreases after peaks;
this is a finite observation, not proof that no memory leak exists.

### Comparison with Xbox and PlayStation

Steam already uses the shared MediaPipeline/backend, parameter-set/IDR recovery,
stream progress watchdogs and performance overlays. Its bounded low-latency queue
uses six packets, 8 MiB and 100ms, matching the PS bounded path's byte/age limits
(PS packet count is derived from FPS). Xbox selects RealtimeQueued, so copying
that scheduling mode blindly would not establish IHS transport correctness.
Steam-specific assembly expiry and sequence-gap recovery happen before the shared
pipeline; they require their own fault-injection tests. None of these desktop
tests executes Switch NVDEC, deko3d presentation or Borealis UI. Hardware stability
and the prior application's exit still need real-device evidence/crash diagnostics.
