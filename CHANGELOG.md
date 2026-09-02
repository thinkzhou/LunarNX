# Changelog

Notable LunarNX changes are recorded here by the automated release workflow.

## [0.3.0](https://github.com/thinkzhou/LunarNX/compare/v0.2.0...v0.3.0) (2026-09-02)

LunarNX 0.3.0 is a major streaming-stability update for Xbox and PlayStation. It improves controller responsiveness, audio/video recovery, PlayStation pairing and startup, rendering correctness, and the overall in-stream experience.

### Xbox streaming

- Added network-aware streaming policies for low-latency local play and more variable cloud connections.
- Improved recovery from burst packet loss, high RTT, RTP gaps, token refresh failures, and long-running video stalls.
- Made controller delivery more reliable under data-channel backpressure, reducing sticky or missing button transitions.
- Stabilized low-latency audio playback and recovery after audio-source resets.

### PlayStation streaming

- Expanded local PS4 and PS5 discovery, pairing, wake-up, and connection handling, including offline-activated consoles and phone-assisted pairing.
- Improved saved-console routing and host probing so previously paired consoles can reconnect more reliably.
- Hardened PSN and local session startup, teardown, first-frame handling, decoder recovery, and packet-loss recovery.
- Improved high-bitrate playback, including 30 Mbps streams, while keeping PlayStation buffering and recovery independent from Xbox policy.
- Restored Remote Play vibration on Switch, including PS5 DualSense haptics and legacy rumble, with clean shutdown when a stream ends.

### Video, audio, and controls

- Fixed the green line at the bottom of 720p video by keeping NV12 storage padding out of visible texture dimensions.
- Fixed false renderer-stall recovery when opening in-stream settings or button mapping, preventing unintended disconnects and crashes while keeping transport and decoder recovery active.
- Separated Xbox and PlayStation video paths so each protocol can apply its own buffering and recovery policy.
- Improved audio/video queue handling, hardware-decoder acceptance checks, and keyframe recovery after corruption or loss.
- Added independent Xbox and PlayStation button-mapping profiles.
- Prevented the in-stream quick menu from leaking input to the remote console and made stream exit progress clearer.

### Interface and distribution

- Redesigned Xbox Cloud Gaming library browsing.
- Rebuilt the in-app About area with controller-friendly Project, Changelog, Community, and Support pages.
- Added direct project and release links, QQ and Discord community access, and optional support QR codes.
- Added clearer update download progress, speed, and ETA reporting.
- Added versioned GitHub releases with NRO and ZIP packages plus SHA-256 checksums.
- Added a Sphaira app-store banner and refreshed the English and Simplified Chinese installation and feature documentation.
- Added clearer diagnostics for connection, media startup, loss recovery, and shutdown failures.

### Notes

- LunarNX remains early-stage homebrew software. PlayStation local pairing and streaming need broader testing across console models, firmware versions, and network environments.
- Launch LunarNX through Homebrew Menu title override/full-memory mode; Applet Mode does not provide enough memory for reliable streaming and hardware decoding.

## 0.2.0 (2026-08-14)

This version is the baseline for automated releases.
