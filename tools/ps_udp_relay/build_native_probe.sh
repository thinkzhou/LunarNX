#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD=${LUNARNX_PSN_PROBE_BUILD:-/tmp/lunarnx-psn-relay-native-build}

if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required to build the native PSN probe" >&2
    exit 1
fi

BREW_PREFIX=$(brew --prefix)
PKG_CONFIG="$BREW_PREFIX/bin/pkg-config"
if [ ! -x "$PKG_CONFIG" ]; then
    echo "Homebrew pkg-config is required to build the native PSN probe" >&2
    exit 1
fi

CMAKE_PREFIX_PATH=
PKG_CONFIG_PATH=
for dependency in json-c miniupnpc libevent opus openssl@3; do
    prefix=$(brew --prefix "$dependency")
    CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH:+$CMAKE_PREFIX_PATH;}$prefix
    PKG_CONFIG_PATH=${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$prefix/lib/pkgconfig
done
export PKG_CONFIG_PATH

cmake -S "$ROOT/tools/ps_udp_relay/native_probe" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH" \
    -DPKG_CONFIG_EXECUTABLE="$PKG_CONFIG"
cmake --build "$BUILD" --target lunarnx-psn-remote-probe -j8
printf '%s\n' "$BUILD/lunarnx-psn-remote-probe"
