#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
IMAGE=${LUNARNX_DEVKIT_IMAGE:-devkitpro/devkita64:20251117}
OUT="$ROOT/build/chiaki-recv-ab"
mkdir -p "$OUT"

build_variant()
{
    mode=$1
    name=$2
    CHIAKI_RECV_OPT="$mode" "$ROOT/tools/chiaki_switch/build_in_docker.sh"
    docker run --rm --platform linux/amd64 \
        -v "$ROOT:/work" -w /work "$IMAGE" bash -lc '
set -e
export DEVKITPRO=/opt/devkitpro
export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
make -f Makefile.switch clean
make -f Makefile.switch -j$(nproc) \
    NETWORK_DIAG=0 CURL_PROVIDER=moonlight CURL_VERIFY=0 \
    CURL_VERBOSE=0 CURL_TIMEOUT_MS=30000
'
    cp "$ROOT/build/switch/LunarNX.nro" "$OUT/$name"
    shasum -a 256 "$OUT/$name"
}

build_variant 0 LunarNX-chiaki-recv-control.nro
build_variant 1 LunarNX-chiaki-recv-optimized.nro

echo "A/B artifacts written to $OUT"
