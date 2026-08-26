# Real-hardware log analysis — 2026-08-26

## Scope and status

- Source log: `/Users/zhouyang/Downloads/lunarnx[2].log`
- Distributed package confirmed by the user:
  `/Users/zhouyang/Downloads/LunarNX-0826-1.nro`, SHA-256
  `5555344ee40fde0eaafeec5c2484cc6c2f5058a9dd0c6a363c910953f89bc003`.
- The dependency-patch cleanup remains deferred. No runtime patch was changed
  as part of this investigation.
- The log file is append-only and does contain a version/process boundary:
  line 1 is the `userAppExit begin` tail of an earlier process, while line 2
  starts the only complete process in this capture. The three stream sessions
  analysed below all belong to that one process and therefore to one loaded
  NRO; they are not three mixed package versions.
- The distributed NRO identifies itself as `LunarNX G9 Realtime`. Its original
  build record uses `APP_VERSION=0.2.0-g9`,
  `GIT_COMMIT=G9-Realtime-20260826`, `DROP_DIAG=1`, and
  `LATENCY_DIAG=0`. Direct binary inspection agrees: it contains the
  `[drop-diag ...]` and `fresh-reconnect` format strings but contains neither
  `[latency-diag ...]` nor the latency session-start format string. This is
  conclusive evidence that the distributed package was intentionally compiled
  as drop-only; it was not the earlier `G8 LatencyTrace` build.
- The active process likewise produced drop diagnostics but no latency
  diagnostics. Only nine asynchronous diagnostic records were reported
  dropped, so the absence of every `[latency-diag ...]` record cannot be
  explained by writer overflow.
- Input sample-to-wire latency and the complete per-stage media latency cannot
  be reconstructed from this log.

## Timeline

1. Line 1 belongs to an earlier app process. Starting at line 2, the app starts
   once. There is no later `userAppInit`, crash, exception, or process restart.
2. Three cloud sessions are started. The first two end with
   `phase=session-cleanup normal=1`.
3. The third session runs for about 47.5 minutes. At diagnostic time
   `t=2604444ms`, an `InputLatest` DataChannel send fails after libpeer has
   already marked SCTP disconnected:

   ```text
   sctp-fatal-send-failure type=1 result=-1 transient=false
   recoverable=false data_channel_connected=false socket_errno=0
   action=reconnect
   ```

4. Local transport quiesce and media-source reset finish in about 17 ms. A new
   cloud session is established about 64.6 seconds later, and media is enabled
   another 625 ms later. Streaming then continues until a normal cleanup.

## Findings

### The server does not honor the requested 1080p ceiling

All three starts request 1920x1080. The first decoded frame is 2560x1440, even
though LunarNX also sends 1920x1080 in the Xbox capability messages and
advertises H.264 `max-fs=8160` in SDP. During the long session the incoming
resolution later changes as follows:

```text
2560x1440 -> 1280x720 -> 1920x1080 -> 2560x1440
```

This is remote adaptation/selection, not a wrong width or height passed into
`XboxStreamSession`. The 1440p packet rate increases loss-recovery and buffering
pressure on the Switch, even though the output framebuffer is only 1280x720.

### Network loss, not local decode/GPU overload, causes the visible stalls

For the long session before reconnect:

- RTT: median 73 ms, P95 134 ms, maximum 580 ms.
- Aggregate detected packet loss: about 2.86%.
- Aggregate packets not recovered inside the accounting window: about 0.18%.
- 688 RTP-loss episodes and 691 rejected/incomplete H.264 access units.
- Recorded PLI-to-displayed recovery: median 541 ms, P95 2.218 s, maximum
  5.292 s.

After reconnect, detected loss is about 3.19% and unrecovered loss about 0.33%.
The log records another 153 RTP-loss episodes in roughly 7.7 minutes.

The current receiver does recover many gaps. Deterministic regressions confirm
that a recorded 52-packet gap is covered by four initial NACKs and a
153-packet gap by nine NACKs over about 59 ms. When retransmission is too late,
however, dependent frames are discarded while waiting for a new IDR. The long
session reaches 2,872 such resync discards before reconnect.

Three severe burst episodes overflow the bounded jitter buffer and request a
decoder reset. GPU fences are retired and streaming resumes after the IDR; the
corresponding freezes are roughly 1.3–1.5 seconds. These are recovery actions,
not GPU hangs.

At the same time, all of these local failure counters remain zero:

- RTP owner-queue drops
- decoder errors
- GPU queue errors
- decoded-frame local, sync, and renderer-queue drops
- SRTP authentication and replay-old failures

The SRTP failures in the log are exclusively `srtp_replay`; they are duplicate
packets/retransmissions and are not authentication corruption.

### The DataChannel association closes independently of media

`type=1` is `OutboundType::InputLatest`, so the failure is observed when the
next complete controller snapshot is sent. It is not evidence that the input
coalescing policy randomly omitted a button state. At that moment
`data_channel_connected=false`; the association had already closed.

With the active usrsctp implementation, the connected flag becomes false on
`SCTP_COMM_LOST` or `SCTP_SHUTDOWN_COMP`. The current log does not retain the
association notification state or `sac_error`, so it cannot distinguish:

1. retransmission timeout/association loss caused by the unstable path; or
2. a graceful/remote control-channel shutdown.

RTP was still arriving shortly before the failure, so this was not a local app
crash or a persistent GPU/media failure. The current fresh-session recovery
works, but its user-visible outage is about 65 seconds because it recreates and
provisions the cloud session; local teardown contributes only about 17 ms.

### Bitrate control reacts, but oscillates

The long session makes 62 REMB target changes, mostly between 20 and 15 Mbps
with occasional 10 Mbps periods. Recovery to a higher tier can happen after
only two clean windows following a single reduction, so bursty Wi-Fi/WAN loss
causes repeated down/up transitions. This does not explain the SCTP close by
itself, but it can add resolution/quality churn and repeatedly return a 1440p
stream to a packet rate the path cannot sustain consistently.

## Code optimization plan

The external network is the trigger, but the client still has four
code-controlled amplifiers: an overly patient recovery policy during burst
loss, a bitrate controller that probes upward too quickly, a permanently
resilient cloud presentation/audio policy, and a full cloud-session rebuild
after an SCTP-only failure. The decoder and GPU are not the optimization target
for this trace.

The changes should be delivered in the following order. Each phase has its own
feature flag and deterministic A/B test so that a latency improvement cannot be
accepted by silently trading it for more freezes, audio underruns, or missed
input transitions.

### Phase 0: make every delay attributable

Affected areas:

- `src/diagnostics/`
- `src/stream/perf_stats.h`
- `src/webrtc/video_rtp_jitter_buffer.cpp`
- `src/webrtc/peer_manager.cpp`
- the legacy libpeer patch under `tools/libpeer_legacy/`

Required instrumentation:

1. Emit version, commit, build flags, and a short package ID at startup even
   when optional diagnostics are disabled.
2. Assign an ID to each video recovery episode and record these timestamps:
   first missing packet, every NACK, incomplete-AU abandonment, first PLI,
   repeated PLI, first recovery-IDR RTP packet, completed IDR AU, decoded IDR,
   renderer submission, and GPU fence retirement.
3. Add one-Hz bounded summaries for RTP-to-AU, AU queue, decode, decoded-frame
   queue, renderer submission, audio queue, network-pump gap, and
   input-sample-to-wire latency. Do not log every packet or frame.
4. Persist usrsctp `sac_state`, `sac_error`, retransmission/flight/queued-byte
   counters, last successful SCTP receive, and adjacent ICE/DTLS transitions.
5. Record every decoded-resolution transition and feed the current pixel rate
   to adaptation telemetry.

All runtime records must continue through the existing bounded asynchronous
writer. The diagnostic build is accepted only if its callback enqueue P99 is
below 100 microseconds, writer drops stay below 0.1%, and the clean-stream A/B
shows no material frame, input, or audio regression.

### Phase 1: shorten loss-recovery tails without damaging clean video

Affected areas:

- `src/webrtc/video_jitter_policy.h`
- `src/webrtc/video_rtp_jitter_buffer.cpp`
- `src/app/xbox_stream_session.cpp`

`frame_hold_ms` is not a fixed delay for every frame: complete frames are
emitted immediately. It is the abandonment deadline for an incomplete head
frame. Therefore the correct change is a recovery state machine, not a blanket
removal of jitter buffering.

Implement three recovery states:

1. **Normal:** complete AUs pass immediately. A missing packet gets an initial
   deadline based on raw RTT plus a small variance margin.
2. **Repairing:** retry NACK only while its predicted return time is earlier
   than the current freeze budget. Track NACK success EWMA by RTT bucket.
3. **Resync:** once repair is no longer likely to arrive in time, abandon the
   damaged dependency chain, request an IDR, and prevent later P frames from
   extending head-of-line blocking.

For good/fair cloud paths, start replay tuning around an 80--220 ms missing
deadline, at most three blocked frames, and a 100--140 ms head-block budget.
Poor/high-RTT paths retain a larger 250--400 ms repair window. Recovery-IDR
candidates keep the existing larger ceiling so that a partially received IDR
is not discarded too aggressively. These are tuning ranges, not constants to
ship before trace replay.

Replace the single one-second PLI cadence with a bounded episode scheduler:
send immediately on entry to hard recovery, retry near 300 ms and 800 ms if no
recovery IDR has arrived, then fall back to one-second retries. Stop on the
first assembled IDR. This avoids waiting a full second after a lost PLI while
also preventing an RTCP request storm.

Replay acceptance targets for this trace:

- PLI-to-present P95 below 1.2--1.5 seconds, versus 2.218 seconds now;
- maximum recovery below 2.5 seconds, unless the trace contains no server IDR;
- at least 50% fewer freezes longer than one second;
- no increase in H.264 corruption reaching the decoder, decoder errors, or
  local queue drops.

### Phase 2: stop REMB and resolution oscillation

Affected areas:

- `src/app/adaptive_bitrate_controller.h`
- `src/app/xbox_stream_session.cpp`
- `src/webrtc/network_path_estimator.*`

The current two-clean-window raise after a single reduction is too aggressive
for this bursty route. Replace it with asymmetric adaptation:

- lower quickly on unrecovered loss, repeated hard-recovery episodes, queue
  growth, or RTT inflation;
- raise in smaller steps only after 10--20 clean windows;
- apply a 15--30 second cooldown after a hard recovery;
- remember a session ceiling and do not immediately probe through the bitrate
  that just failed;
- distinguish repaired RF loss from congestion, but count repeated NACK/PLI
  recovery cost even when final packet loss is reported as zero.

The Xbox server may ignore the requested 1080p ceiling, so adaptation must also
protect the receiver from the observed stream. If decoded video exceeds the
requested resolution, cap the next REMB probe conservatively and require a
long stable interval before lifting that cap. A 2560x1440 stream with repeated
loss episodes must not immediately return to 20--30 Mbps merely because two
one-second windows were clean.

Acceptance targets on the same-duration replay are fewer than ten target
changes instead of 62, no persistent 20/15 Mbps ping-pong, and at least a 30%
reduction in incomplete-AU/resync episodes without reducing clean-path display
FPS.

### Phase 3: use dynamic latency instead of permanent cloud buffering

Affected areas:

- `src/app/xbox_latency_policy.h`
- `src/stream/realtime_latency_policy.h`
- `src/stream/media_pipeline.cpp`
- `src/stream/video_renderer.cpp`
- `src/stream/audio_player.cpp`
- `src/stream/av_sync.cpp`

Cloud currently always selects `BufferedFifo`, resilient decode catch-up, and a
125 ms Audren capacity. Add a hysteretic runtime policy with three modes:

| Mode | Entry condition | Video | Audio target |
| --- | --- | --- | --- |
| Realtime | stable RTT/loss and no recent recovery | latest eligible decoded frame at final submit; dependency-order decode retained | about 80--100 ms |
| Balanced | ordinary cloud variation | one-frame FIFO | about 100 ms |
| Recovery | loss burst, RTT inflation, or recent PLI | resilient catch-up and bounded FIFO | up to 125 ms |

Require several clean seconds to move toward lower latency and switch to
Recovery immediately on damage. Return gradually so a single good/bad window
cannot flap the presentation or audio policy.

Do not recreate Audren whenever the mode changes. Keep fixed capacity, control
the target queue depth with low/high watermarks, and add a small clock-skew
servo. Shed only excess old PCM after the high watermark. The existing model
shows that blindly changing 125 ms to 80 ms improves clean latency by roughly
20 ms but produces underruns on 90--150 ms gaps, which is why dynamic queue
depth is required.

For video, select the newest safe decoded frame close to the final GPU submit
in Realtime mode. Never skip H.264 compressed frames before decode because
later P/B frames may depend on them. Balanced and Recovery modes preserve one
frame of elasticity.

Do not make the existing AV-sync drop branch reachable by merely changing
`< -200 ms` to `<= -200 ms`. The lateness value is already clamped, and the
previous rollback showed that unconditional dropping is unsafe. Instead:

- retain the unclamped timing error for diagnostics;
- rebase the media clock on source/reconnect/discontinuity events;
- slew small audio/video drift;
- discard a late decoded presentation only when a newer safe frame is already
  available; never discard the sole latest frame.

Expected clean/fair-path gain is roughly 8--15 ms from final video selection
and 20--30 ms from adaptive audio depth. The diagnostic build must measure the
actual RTP-to-submit and input-to-wire latency before these values are treated
as hardware results.

### Phase 4: recover controls independently and prevent short-tap loss

Affected areas:

- `src/app/xbox_stream_session.cpp`
- `src/app/xbox_channel_manager.cpp`
- `src/webrtc/peer_manager.cpp`
- legacy libpeer SCTP integration

First add association-state evidence from Phase 0. Then implement a guarded
fast path when RTP, ICE, and DTLS remain healthy but SCTP alone closes:

1. detect and expose the control outage immediately;
2. attempt a same-Xbox-session peer/SCTP renegotiation with a 3--5 second
   deadline behind a feature flag;
3. preserve the last rendered video and show a controls-reconnecting state;
4. fall back to the existing fresh cloud-session path if renegotiation is not
   supported or media health also fails.

Do not simply reopen the usrsctp socket inside the old peer: SCTP is bound to
the DTLS/WebRTC negotiation, and the earlier stale-session path caused a black
screen. The 65-second outage can become less than five seconds only if the Xbox
service accepts same-session renegotiation; otherwise this phase still improves
detection, UI behavior, and diagnostics while keeping the proven fallback.

Input loss is a separate issue from an already-dead association. Keep the
8 ms newest full-state snapshot for sticks, but add a tiny bounded transition
journal for short button presses/releases. Expire transitions after 32--50 ms,
never replay stale analog values, and allow one redundant release snapshot.
This retains the non-sticky behavior while preventing a press and release that
occur between successful sends from disappearing entirely.

### Verification matrix and release gates

Extend the existing deterministic tools to replay real timestamped recovery
episodes, then run all of these profiles against old and new policies:

- home RTT: 2, 8, 20, and 50 ms;
- cloud RTT: 35, 70, 120, and 200 ms;
- random loss: 0.1%, 0.5%, 2%, and 5%;
- burst loss/gaps: 50, 100, 300, and 800 ms;
- bandwidth changes: 30 -> 10 -> 20 Mbps;
- RTT spikes to 300--600 ms;
- 80/250 ms DataChannel backpressure and an SCTP-only close;
- 720p/1080p/1440p resolution transitions;
- media-only stall, renderer stall, and source discontinuity.

Score user experience, not only picture quality:

- input sample-to-wire P50/P95/max and missed short taps;
- RTP loss-to-present recovery P50/P95/max;
- displayed FPS, frame-gap P95/max, and freezes over 250/500/1000 ms;
- incomplete AUs, resync discards, PLI count, and decoder/GPU errors;
- audio queue P50/P95, underruns, discontinuities, and A/V drift;
- REMB transition count, received bitrate, and decoded resolution;
- SCTP detection and recovery duration;
- CPU time, network-pump gaps, async-log drops, and all local queue drops.

A phase may ship only when it improves its target metrics and does not regress
the clean home profile, displayed FPS, input correctness, audio continuity, or
GPU/decoder stability. Ryubing trace replay is the regression gate; a real
Switch run with the full diagnostic package remains the final gate.

### Explicit non-solutions

- Do not enlarge queues; that hides loss by adding latency.
- Do not optimize or reset the GPU/decoder for this trace; their error and
  local-drop counters are zero.
- Do not force every cloud path to 80 ms audio or aggressively drop all late
  frames.
- Do not assume REMB forces the Xbox server to choose 1080p.
- Do not remove the legacy libpeer receive-buffer patch or disable SCTP.
- Do not add synchronous per-packet logging to media/input callbacks.

## Next diagnostic build

Before changing recovery policy again, the next diagnostic package should:

1. build with `LATENCY_DIAG=1` as well as `DROP_DIAG=1`;
2. write an always-on startup identity containing version, commit, build flags,
   and a short build/package ID outside the optional latency logger;
3. persist usrsctp association notifications including `sac_state`,
   `sac_error`, outbound queue/flight size, and last successful SCTP receive;
4. persist ICE/DTLS state transitions around a DataChannel close;
5. record the server-selected decoded resolution continuously or on every
   transition; and
6. retain the existing bounded asynchronous writer so logging does not run on
   media/input callbacks.

## Verification performed

The following deterministic desktop regressions pass against the current
working tree:

```text
make -f Makefile.desktop stream_resilience_tests outbound_queue_tests adaptive_bitrate_tests
```

They cover the recorded 52/153-packet loss gaps, recovery-keyframe repair,
outbound input/NACK priority, transient versus fatal SCTP send handling, and
adaptive bitrate state transitions.
