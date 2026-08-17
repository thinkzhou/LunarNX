#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
docker_image="${LUNARNX_DEVKIT_IMAGE:-devkitpro/devkita64:20251117}"
docker_platform="${DOCKER_PLATFORM:-linux/amd64}"

docker run --rm --platform "$docker_platform" \
    -e CHIAKI_RECV_OPT="${CHIAKI_RECV_OPT:-1}" \
    -e CHIAKI_TRANSPORT_DIAG="${CHIAKI_TRANSPORT_DIAG:-0}" \
    -e BUILD_JOBS="${BUILD_JOBS:-}" \
    -v "$project_root:/work" -w /work \
    "$docker_image" \
    bash /work/tools/chiaki_switch/build_in_container.sh
