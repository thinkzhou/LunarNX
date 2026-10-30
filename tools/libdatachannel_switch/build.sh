#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
PLATFORM="${1:-switch}"
python3 tools/libdatachannel_switch/prepare.py
BUILD="build/libdatachannel-sdk/$PLATFORM"
options=(-DCMAKE_BUILD_TYPE=Release)
if [[ "$PLATFORM" == switch ]]; then
  [[ -n "${DEVKITPRO:-}" && -x /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc ]] || {
    echo 'Switch SDK must be built inside devkitpro/devkita64:20251117 Docker.' >&2; exit 1;
  }
  options+=(-DLUNAR_SWITCH=ON -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/libdatachannel_switch/compat/switch.cmake")
elif [[ "$PLATFORM" != native ]]; then
  echo 'Usage: build.sh switch|native' >&2; exit 1
fi
cmake -S tools/libdatachannel_switch -B "$BUILD" "${options[@]}"
cmake --build "$BUILD" --target datachannel-static lunar_rtc_background -j "${JOBS:-6}"
touch "$BUILD/ready.stamp"
