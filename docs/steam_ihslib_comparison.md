# IHSlib comparison: LunarNX and NSteamLink

Compared on 2026-10-12, before LunarNX's transport corrections below:

- LunarNX worktree `LunarNX-steam-desktop`, branch `codex/steam-desktop-validation`,
  application commit `80b0c34`; IHSlib submodule `560908104b1f884792f90d8f98e33ec8c61106f5`
  from [mariotaku/ihslib](https://github.com/mariotaku/ihslib/tree/560908104b1f884792f90d8f98e33ec8c61106f5).
- NSteamLink `main` references [kxn/ihslib
  `04688d19951c68bedc4370234cc1752043632cb3`](https://github.com/kxn/ihslib/tree/04688d19951c68bedc4370234cc1752043632cb3).
  Its fork history includes changes from a different upstream branch and later
  official changes. This is a comparison of the effective sources, not a claim
  that every difference was originally authored by NSteamLink.

LunarNX keeps the pinned vendor checkout pristine and compiles tracked
`src/steamlink/ihs_*.c` overrides. NSteamLink edits its IHSlib fork. Switching
LunarNX wholesale to that fork would also replace callback/ownership contracts,
negotiation behavior and the media lifecycle, so it is not part of this change.

## Source-backed differences

| Area | NSteamLink's effective behavior | LunarNX before this change | Decision |
| --- | --- | --- | --- |
| Reliable registration | Clones/tracks before admitting an initial datagram; retry begins after the initial send | Sender sends first, then transfers the packet into the retry queue; a fast ACK can be missed | Adapt the ordering and independent buffer ownership |
| ACK/NACK | Cumulative ACK; extended NACK releases contiguous/present packets and keeps holes; missing reliable messages survive until ACK/disconnect | ACK and NACK both cancel only an exact packet; timer stops after 20 retries | Adapt cumulative/selective receipt and retain missing reliable packets; cap outstanding packets at 1024 |
| Retry pacing and clock | Rolling ACK clock/RTT estimate; age-gated selective resend, stale feedback filtering, oldest-first batch retries | Fixed 10 ms retries, no peer clock estimator | Use a fixed 25 ms fallback in this bounded patch; adaptive pacing, clock feedback and immediate selective resend remain deferred |
| Reliable receive window | Starts at expected ID 0, preserves initial holes, grows up to 16384 slots; confirms contiguous receipt and emits periodic presence-mask NACK | First arriving ID becomes the origin; fixed 128-slot control window; ACKs the individual arrival | Adapt origin/growth/contiguous ACK; periodic outgoing NACK remains deferred, so recovery still depends on the peer's retry timer |
| Fragmented control sends | Head counts following fragments; each datagram gets its own consecutive packet ID | Head overcounts fragments; every fragment repeats the same packet ID | Correct counts and IDs and replay complete round trips |
| Media packet window | Skips orphan fragments when joining an unreliable stream; ignores already consumed duplicates | Orphan fragments can block the head; exact-tail duplicate can refill a consumed slot | Adapt both fixes without making media reliable |
| Packet routing | Validates peer address/port and connected IDs; permits unconnected discovery MTU probes | Receiver ignores the sender address and dispatches solely by channel | Adapt endpoint/connection checks before processing feedback |
| Channel teardown | Media workers stop/join before control and stats are freed | Session destroys control/stats before media channel destructors | Destroy media first while preserving discovery/control ordering |
| Pairing persistence | Per-host secret and authorized Steam identity; application stores host records atomically | Device ID/key persisted; host authorization maps are in memory and require re-pairing after client recreation | High-value follow-up requiring migration, revoked/stale-host handling and application tests; not a transport-only change |
| HID reports | Negotiated report length, adaptive delta/full encoding, activity reporting, batched ordering, retained device allocations while snapshots exist | Native 48-byte generic pad, full reports on changes, explicit full resync, timer/slot synchronization and close/reopen race tests | Keep safe full reports; study delta/activity/length semantics separately, coupled with reliable transport tests |
| Media identity/start | Waits for StartVideoData and applies application-provided decoder/system capabilities; can buffer early packets | StartVideoData creates channels; controller initializes the media pipeline earlier and reports a hardcoded Marvell decoder and unrelated TV/monitor system description | Remove false decoder/TV/monitor identity; early packet buffering and application-provided capability negotiation remain follow-ups |
| Frame feedback | Peer clock translation, tracked tickets through decode/GPU completion, transactional frame-stat snapshots | Records local receive and synchronous submit timings, sends accumulated stats; no frame ticket through actual GPU completion | Valuable but cross-layer work; do not copy stats that would claim a frame was presented before GPU completion |
| Stop/resume and lifecycle | Explicit host video-stop/session-stop state, decoder domains and ownership contracts | Existing connection/first-frame/media/HID watchdogs, presentation suspension, drained cancel/retry, input gating and menu lifecycle | Keep existing protections; inspect host video pause/resume semantics in a separate integration change |

Sources for NSteamLink behavior: pinned fork
[retransmission.c](https://github.com/kxn/ihslib/blob/04688d19951c68bedc4370234cc1752043632cb3/src/session/retransmission.c),
[window.c](https://github.com/kxn/ihslib/blob/04688d19951c68bedc4370234cc1752043632cb3/src/session/window.c),
[channel.c](https://github.com/kxn/ihslib/blob/04688d19951c68bedc4370234cc1752043632cb3/src/session/channels/channel.c),
[ch_control.c](https://github.com/kxn/ihslib/blob/04688d19951c68bedc4370234cc1752043632cb3/src/session/channels/ch_control.c),
[session.c](https://github.com/kxn/ihslib/blob/04688d19951c68bedc4370234cc1752043632cb3/src/session/session.c),
[video channel](https://github.com/kxn/ihslib/blob/04688d19951c68bedc4370234cc1752043632cb3/src/session/channels/video/ch_data_video.c),
and NSteamLink's [fork notes](https://github.com/kxn/nsteamlink/blob/main/third_party/README.md)
and [technical guide](https://github.com/kxn/nsteamlink/blob/main/docs/TECHNICAL.md).

## Scope and validation

Transport changes are tracked overrides, retaining LGPL notices and the vendor
revision. Switch and desktop probe source lists must use the same overrides.
The production-source sanitizer replay covers cumulative ACK, missing NACK,
selective receipt, ACK before initial-send completion, retry lifetime, ID wrap,
receive holes/growth, orphan media and fragmented round trips. Existing Steam
HID/client/runtime tests exercise regression behavior separately.

These local replays do not authenticate with Steam, run a decoder, or present
Switch GPU frames. The Xbox Ryubing mock uses another transport and cannot prove
Steam protocol interoperability. Real Steam/Switch verification must include
pairing, picture/audio/input, induced packet loss, disconnect/reconnect and two
successive hbmenu launches. Simulator/build evidence and any remaining gaps are
recorded with the integration notes in `steam_link.md`.
