#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE="devkitpro/devkita64:20251117"
INSTALL=0

if [[ "${1:-}" == "--install" ]]; then
    INSTALL=1
elif [[ $# -ne 0 ]]; then
    echo "usage: $0 [--install]" >&2
    exit 2
fi

docker run --rm --platform linux/amd64 \
    -e FFMPEG_JOBS="${FFMPEG_JOBS:-}" \
    -v "$PROJECT_DIR:/work" -w /work \
    "$IMAGE" \
    bash /work/tools/ffmpeg_switch_build/build_in_docker.sh

if [[ $INSTALL -eq 1 ]]; then
    cp "$PROJECT_DIR/build/ffmpeg-switch/install/lib/libavcodec.a" \
       "$PROJECT_DIR/lib/switch/libavcodec.a"
    echo "Installed patched libavcodec.a to lib/switch"
fi
