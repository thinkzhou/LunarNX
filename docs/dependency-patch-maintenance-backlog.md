# Dependency Patch Maintenance Backlog

Status: deferred on 2026-08-26. Do not reorganize or remove the current patch
chain while the latest real-hardware Xbox streaming build is under test.

## Current patch map

The canonical Switch dependency setup applies these patches in order:

1. Borealis: `tools/borealis_switch/lunarnx-borealis-gpu-lifecycle.patch`.
2. Legacy libpeer:
   - `tools/libpeer_legacy/0001-switch-adapt-libpeer-WebRTC-path.patch`
   - `tools/libpeer_legacy/0002-fix-H264-access-unit-flush-and-quiet-SCTP-logs.patch`
   - `tools/libpeer_legacy/legacy-libpeer-switch.patch`
   - `tools/libpeer_legacy/0003-enable-usrsctp-datachannel-policy.patch`
3. Legacy libpeer submodules:
   - libsrtp network-byte-order compatibility
   - mbedTLS DTLS-SRTP and ECDSA certificate compatibility
   - usrsctp libnx/BSD compatibility and Switch CSPRNG

The separately built Chiaki SDK applies ten focused patches listed by
`tools/chiaki_switch/build_in_container.sh`. The older monolithic
`lunarnx-chiaki-switch.patch` is not part of that canonical build. The custom
FFmpeg build applies `nvtegra-status-clear.patch` after the pinned wiliwili
patches.

## Findings to preserve for the later cleanup

- The current clean-checkout patch order is reproducible and produces the
  known-working source tree. Do not delete patches independently from the live
  chain.
- The fully patched legacy libpeer checkout is not idempotently recognized by
  `scripts/setup_dependencies.sh`: forward and reverse checks fail for the
  overlapping `0001`, `0002`, and `legacy-libpeer-switch.patch` layers.
- `legacy-libpeer-switch.patch` is nearly 4,000 lines, contains repeated diff
  blocks for the same files, and mixes ICE, DTLS, SDP, media receive, feedback,
  socket backpressure, diagnostics, and later transport fixes.
- `0002` is a removal candidate. The active Xbox path sets `raw_video_rtp=1`, so
  its libpeer H.264 depacketizer fix is bypassed; a clean patch replay also
  succeeds when `0002` is omitted. Keep it until a focused build/regression pass
  confirms removal is behavior-neutral.
- The libsrtp Switch compatibility intent is required: a clean Switch
  cross-build without it fails because `ntohl`, `ntohs`, `htonl`, and `htons`
  are undeclared. Its patch text contains duplicate includes and can be reduced
  to a minimal compatibility change.
- The mbedTLS patch must preserve DTLS-SRTP enablement and standards-compliant
  ECDSA AlgorithmIdentifier generation. CMake default changes, duplicated
  comments, and comment-only punctuation edits should be removed.
- The usrsctp patch must preserve the Switch `sockaddr_conn` layout, missing BSD
  compatibility definitions, and `randomGet()` entropy path.
- The 16-entry local SCTP stream-policy table is defensive rather than required
  by today's four Xbox channels, but is low-cost and aligned with Green-NX.
- The raw-RTP path bypasses libpeer's built-in H.264 assembler, but both audio
  and video `RtpDecoder` instances still contain two 2 MiB H.264 arrays. A later
  cleanup should remove or dynamically allocate this inactive fallback storage.
- Chiaki transport and key-position diagnostic patches are diagnostic-only in
  release builds. The inactive monolithic Chiaki patch and its old helper/tests
  should be retired only after their remaining references are migrated.
- FFmpeg `nvtegra-status-clear.patch` is primarily a Ryubing compatibility fix;
  retain it unless simulator support is deliberately dropped.

## Proposed no-behavior-change cleanup

1. Freeze a known-good source revision and real-hardware artifact.
2. Add a manifest recording every dependency revision, patch purpose, runtime
   path, and protecting regression test.
3. Regenerate legacy libpeer as small thematic patches: Switch portability,
   Xbox ICE/DTLS/SDP, media receive/feedback, usrsctp DataChannel policy, and
   optional diagnostics.
4. Remove dead fallback hunks and reduce nested patches to their semantic
   minimums.
5. Make dependency setup idempotent using a final-tree marker/hash rather than
   reverse-applying overlapping historical patches.
6. Re-run clean Docker builds, focused regressions, BSS checks, and real-hardware
   A/B validation before changing the release patch chain.
