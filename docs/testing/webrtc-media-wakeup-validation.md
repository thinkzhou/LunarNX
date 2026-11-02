# Event-driven media handoff validation

2026-11-02. Follow-up to experimental backend PR #37, based on `641bb9e`.
These are same-host native measurements, not Switch performance claims.

## Implementation

libdatachannel producers notify a condition variable when the inbox changes
from no actionable work to actionable work. Queue inspection and enqueue use
the same mutex; notifications happen after unlock and coalesce while work is
pending. Existing stream-owner callbacks, queue limits and startup gating remain.
The owner waits until work arrives or its normal 2-ms service deadline expires.
The deadline is anchored to the iteration start, so processing time is not
added to the idle input interval and early media wakeups cannot accumulate a
future deadline. Input sampling still runs independently every 8 ms.

Failure notifications use the same mutex. Once `loop()` reports a latched
failure, it no longer keeps the readiness predicate permanently true. Close
notifies waiters; normal shutdown joins the owner before destroying its peer.
Cancellation still has at most a 2-ms idle wait. Legacy retains its polling path.

## Method

- Same pinned PR #37 SDK and legacy checkout as the prior integrated mock test.
- Real Xbox Home REST/SDP/ICE/DTLS/SRTP, H.264/Opus and all four data channels;
  periodic neutral gamepad packets are encoded, sent and validated by the mock.
- 720p60 testsrc2 video plus audio. Mock encoding target frozen at 6 Mbps to
  prevent different bandwidth feedback from changing the workload. Actual
  measured video RTP rate was approximately 12.1–12.2 Mbps, not 6 Mbps.
- One instrumented native build compares owner polling with event waiting
  disabled against event waiting enabled. The production change has no test
  environment switch; the override exists only in the local benchmark overlay.
- Each group runs `new, legacy, legacy, new, new, legacy`, giving three sessions
  per backend per group, twelve sessions total. Polling group precedes wakeup
  group; host load and power states are not isolated. No builds run during the
  measurement windows.
- Sessions run about 20 seconds after start returns, with the first 2 seconds
  excluded. Sender timestamps precede SRTP sending; receiver timestamps are at
  raw library callback entry, enqueue, dequeue and shared application callback.
  Sidecar records match SSRC, RTP sequence and timestamp on the same host clock.
- CPU uses process-wide user+system time across every client thread divided by
  measurement wall time; 100% is one logical core. Sender CPU is excluded.
- All video windows exclude the final 50 ms before shutdown, uniformly. The
  untrimmed first wakeup-group legacy sample had 19 packets sent within 0.4 ms
  of shutdown that never reached the application callback. They are retained
  in the raw evidence and are not silently reported as successful delivery.
- Input delay uses the original monotonic-millisecond protocol timestamp to
  mock receive callback; quantization is approximately 1 ms. This is neither
  physical controller latency nor game-response latency.

## Results

Each value is the median of three per-session statistics, including the
percentiles; it is not a percentile calculated from pooled samples.

| Metric | New backend, polling | New backend, event wakeup |
| --- | ---: | ---: |
| Sender → application callback P50 | 1.409 ms | 0.415 ms |
| Sender → application callback P95 | 2.774 ms | 0.764 ms |
| Sender → application callback P99 | 3.094 ms | 1.034 ms |
| Adapter queue residence P95 | 2.337 ms | 0.256 ms |
| Sender → raw library callback P95 | 0.710 ms | 0.696 ms |
| Client CPU, one-core percent | 19.037% | 19.851% |
| Input delivery P95 | 1.266 ms | 1.354 ms |
| Input delivery P99 | 1.688 ms | 1.653 ms |

Video P95 decreases about 72.5%, and queue P95 about 89.1%. CPU increases
0.813 percentage points (about 4.3% relative), rather than decreasing. Three
samples and partly overlapping CPU ranges do not establish a stable CPU cost.
Input changes are small compared with timestamp quantization; no input-latency
improvement is claimed.

The unchanged legacy control has P95 2.609 / 2.627 ms and CPU 16.394 / 16.151%
in the two groups. Event-driven new-backend delivery is therefore faster than
legacy polling in this test, but still uses more receiver CPU.

In the uniformly trimmed video windows, sent/paired packets are:

| Group | Legacy | New backend |
| --- | ---: | ---: |
| Polling | 64,221 / 64,221 | 64,105 / 64,105 |
| Wakeup | 64,131 / 64,131 | 64,100 / 64,100 |

All twelve sessions record zero video decode errors and video queue drops;
video, audio and input continue progressing. Stop takes 129–166 ms. The probe
counts media-pipeline progress, not physical screen presentation. Raw audio
RTP is not preserved at the shared callback, so no audio packet-latency claim
is made. Buffered per-packet diagnostics contribute to measured CPU.

Generated evidence is retained under `build/` in the local benchmark worktree:
`wakeup-baseline.log`, `wakeup-enabled.log`, both run directories (receive/stage
CSV and app logs), `wakeup-send.csv`, and the corresponding `*-results.json`.
The overlay uses `BENCH_DISABLE_EVENT_WAKE=1` only for the polling group;
`tools/mock_xbox/analyze_stages.py` performs timestamp pairing and aggregation.
Generated logs, CSV, source overlays and binaries are not committed.

## Verification and limits

- Docker `devkitpro/devkita64:20251117` dual-backend NRO builds and BSS check
  pass at 20.4 MiB. Missing Chiaki SDK was rebuilt inside Docker before linking.
- Native Home/cloud and backend-switch lifecycle probe passes all four cycles;
  stop is 136–153 ms. Native idle/consumed-failure/close wait regression passes.
- Enabled/disabled configuration, outbound queue, resilience and input
  accumulator tests pass. Focused session-order, SCTP, DTLS and PPID checks pass.
- `git diff --check` passes. Independent concurrency review identified failure
  busy-loop and notification issues; both were fixed and re-reviewed. Its
  cadence concern was addressed by anchoring deadlines to iteration start.
- A full Ryubing mock-stream smoke was not run: neither the documented Canary
  app/archive nor the legacy headless executable was available on this host.
  Native mock streaming above does not substitute for emulator/hardware results.
- Real Switch condition-variable behavior, CPU/power, WAN loss/recovery,
  display/audio latency and long sessions remain unverified. The production
  default remains legacy libpeer.
