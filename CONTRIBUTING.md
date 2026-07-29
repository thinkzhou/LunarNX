# Contributing to LunarNX

LunarNX prioritizes reliable streaming on real Nintendo Switch hardware.
Simulator results are useful for regression testing, but real hardware is the
final compatibility target.

## Before submitting a change

- Keep tokens, authentication files, logs, simulator data, and build artifacts
  out of commits.
- Do not include personal IP addresses or unredacted Xbox API responses in
  issues, tests, or documentation.
- Build Switch-targeted code only inside the pinned devkitA64 Docker image.
- Keep the active WebRTC implementation on the legacy `lib/libpeer` path unless
  a change explicitly targets another provider.
- Preserve the Switch NRO BSS regression guard of less than 32 MiB.

## Setup and build

Follow the [development guide](docs/development.md) for dependency setup and
build commands. The release-style Switch configuration keeps application and
network diagnostics disabled.

## Validation

At minimum, run the focused tests for the area you changed and:

```sh
python3 tests/switch_nro_bss_test.py
git diff --check
```

Changes to Switch code or build files require a clean Docker build. Streaming
changes should also receive a Ryubing mock-stream smoke test and, whenever
possible, a real-hardware test.

## Pull requests

Describe the user-visible result, tests performed, and any remaining simulator
or real-hardware risk. Avoid attaching complete runtime logs; quote only the
smallest redacted section needed to explain a failure.
