# XStreaming Signaling Polling Alignment

## Goal

Align LunarNX SDP and ICE response polling cadence with XStreaming while
preserving LunarNX cancellation and bounded-failure behavior.

## Reference Behavior

XStreaming polls both SDP and ICE exchange endpoints at one-second intervals.
LunarNX currently waits only 100 milliseconds between attempts. The faster
cadence can consume an early, partial Xbox ICE response before later public or
Teredo candidates are available.

## Design

- Use one shared `1000ms` signaling poll interval for SDP and ICE response
  polling in `XboxSessionClient`.
- Keep the existing maximum of 20 attempts. LunarNX must still return a clear
  error instead of waiting indefinitely as XStreaming currently can.
- Use the provided cancellable sleep callback for both SDP and ICE polling so
  leaving the flow interrupts the wait promptly.
- Accept the first non-empty, parseable response exactly as XStreaming does.
  Do not add an extra candidate-stability window in this change.
- Do not change REST payloads, ICE candidate rewriting, WebRTC timeouts, or the
  libpeer checklist implementation.

## Error Handling

HTTP and parse failures continue to use the current API error messages. A
response that remains empty for all attempts continues to report the existing
SDP or ICE timeout error. Cancellation remains distinct from timeout.

## Verification

- Add a source regression that requires one shared `1000ms` interval and
  cancellable sleeps in both polling paths.
- Run the Xbox stream session ordering and focused ICE/libpeer tests.
- Build the Switch NRO in Docker with `devkitpro/devkita64:20251117`.
- Run the Ryubing mock stream and confirm ICE, DataChannel, and media still
  connect.
- Run one official Xbox attempt and compare returned candidate completeness;
  real hardware remains the final compatibility target.

