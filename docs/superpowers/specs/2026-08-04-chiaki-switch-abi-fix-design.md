# Chiaki Switch ABI Fix Design

## Problem

The Switch application and `lib/switch/libchiaki.a` are compiled with different
public Chiaki configuration. The library was built with
`CHIAKI_LIB_ENABLE_MBEDTLS`, while LunarNX consumers do not define it. As a
result, public structures such as `ChiakiSession` have different layouts.
`chiaki_session_init()` writes beyond the object allocated by LunarNX and the
real-hardware process stops at that call.

## Design

Treat the local `github_repos/chiaki-ng` checkout as the source of one coherent
Switch SDK. A Docker-only build script rebuilds Chiaki and installs the static
library, public headers, and generated headers into `lib/switch` together. The
application consumes the same public configuration defines used by that build.

Add an ABI fingerprint exported by the Chiaki archive. The fingerprint reports
the library's `sizeof` and critical `offsetof` values. A build-time checker
compares those values with a consumer translation unit compiled using LunarNX's
flags. The Switch build must fail before linking when the SDK is inconsistent.

The obsolete holepunch dependency stubs are removed from the Switch CMake path.
Both Makefile and CMake builds must use real curl 8 WebSocket, json-c, and
miniupnpc dependencies for PSN Remote.

## Verification

- The ABI regression must fail against the current mismatched configuration.
- Rebuild and install the Chiaki SDK in `devkitpro/devkita64:20251117`.
- The ABI regression and PS dependency regressions must pass.
- Perform a clean Switch NRO build in Docker and run the BSS guard.
- Run focused PS, session, WebRTC, and whitespace tests.
- Real Switch testing remains required to validate DATA hole punching and media
  startup after `chiaki_session_init()`.

