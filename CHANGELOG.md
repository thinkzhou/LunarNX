# Changelog

Notable LunarNX changes are recorded here by the automated release workflow.

## [0.4.0](https://github.com/thinkzhou/LunarNX/compare/v0.3.0...v0.4.0) (2026-09-04)

LunarNX 0.4.0 focuses on faster reconnection, safer stream teardown, and
predictable release packaging. It reuses successful Xbox and PlayStation
network routes, improves cancellation and UI lifecycle handling, and publishes
stable assets for Sphaira installation.

### Features

* **ps:** reuse successful remote routes ([ddba4b1](https://github.com/thinkzhou/LunarNX/commit/ddba4b1b2f9d05f996dda794625da412244a44d1))
* **xbox:** reuse successful ICE paths ([a600fe7](https://github.com/thinkzhou/LunarNX/commit/a600fe75474abf85b0f84b1df183df2406d449a1))
* **xbox:** speed up stream exit and reuse ICE paths ([5a6eb0d](https://github.com/thinkzhou/LunarNX/commit/5a6eb0dd2ad57f2869b8a93fb4fb0095ecd4c9e6))


### Bug Fixes

* **ps:** restore remote connection reliability ([5673b42](https://github.com/thinkzhou/LunarNX/commit/5673b4290be0d3acd5b06415d852caa785ec0690))
* **ps:** restore remote reliability and reuse successful routes ([845e8d5](https://github.com/thinkzhou/LunarNX/commit/845e8d51572c26b47ae465ab30b4280e0ce343cc))
* **release:** make versions and packages future-proof ([9ccaef7](https://github.com/thinkzhou/LunarNX/commit/9ccaef75321e7d4cc79cc01fb41aeccc7581c692))
* **ui:** coordinate async teardown with view lifecycle ([e3268a1](https://github.com/thinkzhou/LunarNX/commit/e3268a13408c26579d841132c80b71d3aee76685))
* **ui:** harden stream exit and connection cancellation ([52d203f](https://github.com/thinkzhou/LunarNX/commit/52d203fe971b9390669b46386b2822e5fce3f71e))
* **ui:** harden stream exit rendering ([9f7a012](https://github.com/thinkzhou/LunarNX/commit/9f7a01280db1fca5a5069da487e45c022bbf071b))
* **ui:** preserve Borealis dialog animations ([7223402](https://github.com/thinkzhou/LunarNX/commit/7223402ba93a3d9104b3e141a5d2767542559a7c))
* **ui:** preserve readable generated release notes ([aa86dee](https://github.com/thinkzhou/LunarNX/commit/aa86dee4e6240b14c0f6b7fc229ed028a5ab00fb))
* **ui:** serialize stream connection cancellation ([69dfd2e](https://github.com/thinkzhou/LunarNX/commit/69dfd2ec039d87d82eb6d17eefed1d4d51bfa7b5))
* **ui:** show 0.3.0 release notes in About ([bac78be](https://github.com/thinkzhou/LunarNX/commit/bac78be29f90f8507c3a9539a1c3012cb1ea898e))
* **xbox:** cancel network waits during stream exit ([7ab4ef1](https://github.com/thinkzhou/LunarNX/commit/7ab4ef1b40ecb2861c038dc96257e91fb9407434))


### Documentation

* add Chinese getting-started guide ([9158bc3](https://github.com/thinkzhou/LunarNX/commit/9158bc37055710420a667ee0e1b3cbbc86a35c35))
* add concise QQ group usage notice ([b5ac46c](https://github.com/thinkzhou/LunarNX/commit/b5ac46cbafca77e7fe8d388018ea69ce1adbaaa6))
* clarify stable ZIP extraction path ([5b60e36](https://github.com/thinkzhou/LunarNX/commit/5b60e363bf4f1081e8a95d9e133d702bbcf721c3))
* note legacy release asset names ([5d59d36](https://github.com/thinkzhou/LunarNX/commit/5d59d36596adbb28f005d15a0f2cfd6042c70304))
* refresh project status and references ([2bc7b16](https://github.com/thinkzhou/LunarNX/commit/2bc7b166a3ef772feb65b75b68d2dda010cb267e))
* refresh project status and references ([3abda3a](https://github.com/thinkzhou/LunarNX/commit/3abda3a35305aaeb271bcc9c859080d623b79dc4))
* **release:** document stable Sphaira install paths ([e669feb](https://github.com/thinkzhou/LunarNX/commit/e669feb0a02c52cc90e897084219b9a3513ea41c))
* **ui:** credit XStreaming and PeaSyo ([f61e7c7](https://github.com/thinkzhou/LunarNX/commit/f61e7c7ed287f97454f1d69b5b0ef149c78dec68))

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
