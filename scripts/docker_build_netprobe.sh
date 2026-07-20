#!/bin/bash
set -e

DOCKER_IMG="${DOCKER_IMG:-devkitpro/devkita64:20251117}"
DOCKER_PLATFORM="${DOCKER_PLATFORM:-linux/amd64}"
LUNAR_DIR="$(cd "$(dirname "$0")/.." && pwd)"
NETPROBE_RAW_HTTP="${NETPROBE_RAW_HTTP:-0}"
NETPROBE_CERTINFO="${NETPROBE_CERTINFO:-0}"
NETPROBE_RAW_PUBLIC="${NETPROBE_RAW_PUBLIC:-1}"
NETPROBE_RUN_CURL="${NETPROBE_RUN_CURL:-1}"
NETPROBE_LOCAL_HOST="${NETPROBE_LOCAL_HOST:-}"
NETPROBE_LOCAL_PORT="${NETPROBE_LOCAL_PORT:-18080}"

docker run --rm --platform "$DOCKER_PLATFORM" \
    -v "$LUNAR_DIR:/work" -w /work \
    -e NETPROBE_RAW_HTTP="$NETPROBE_RAW_HTTP" \
    -e NETPROBE_CERTINFO="$NETPROBE_CERTINFO" \
    -e NETPROBE_RAW_PUBLIC="$NETPROBE_RAW_PUBLIC" \
    -e NETPROBE_RUN_CURL="$NETPROBE_RUN_CURL" \
    -e NETPROBE_LOCAL_HOST="$NETPROBE_LOCAL_HOST" \
    -e NETPROBE_LOCAL_PORT="$NETPROBE_LOCAL_PORT" \
    "$DOCKER_IMG" bash -lc '
set -e
export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH

echo "Docker image: '"$DOCKER_IMG"'"
echo "NETPROBE_RAW_HTTP: $NETPROBE_RAW_HTTP"
echo "NETPROBE_CERTINFO: $NETPROBE_CERTINFO"
echo "NETPROBE_RAW_PUBLIC: $NETPROBE_RAW_PUBLIC"
echo "NETPROBE_RUN_CURL: $NETPROBE_RUN_CURL"
echo "NETPROBE_LOCAL_HOST: $NETPROBE_LOCAL_HOST"
echo "NETPROBE_LOCAL_PORT: $NETPROBE_LOCAL_PORT"
aarch64-none-elf-g++ --version | head -1
/opt/devkitpro/portlibs/switch/bin/curl-config --version
echo "switch-curl ssl backend: $(/opt/devkitpro/portlibs/switch/bin/curl-config --ssl-backends)"

make -f Makefile.switch.netprobe clean
make -f Makefile.switch.netprobe -j$(nproc) \
    NETPROBE_RAW_HTTP="$NETPROBE_RAW_HTTP" \
    NETPROBE_CERTINFO="$NETPROBE_CERTINFO" \
    NETPROBE_RAW_PUBLIC="$NETPROBE_RAW_PUBLIC" \
    NETPROBE_RUN_CURL="$NETPROBE_RUN_CURL" \
    NETPROBE_LOCAL_HOST="$NETPROBE_LOCAL_HOST" \
    NETPROBE_LOCAL_PORT="$NETPROBE_LOCAL_PORT"

ls -lh /work/build/switch-netprobe/LunarNXNetProbe.nro
'
