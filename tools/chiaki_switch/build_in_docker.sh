#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
docker_image="${LUNARNX_DEVKIT_IMAGE:-devkitpro/devkita64:20251117}"
docker_platform="${DOCKER_PLATFORM:-linux/amd64}"
chiaki_checkout="${CHIAKI_SOURCE_CHECKOUT:-}"
extra_mount=()
if [[ -n "$chiaki_checkout" ]]; then
    extra_mount=(-v "$chiaki_checkout:/chiaki-source:ro"
                 -e CHIAKI_SOURCE_CHECKOUT=/chiaki-source)
fi

docker run --rm --platform "$docker_platform" \
    "${extra_mount[@]}" \
    -e CHIAKI_SDK_PROFILE="${CHIAKI_SDK_PROFILE:-legacy}" \
    -e CHIAKI_RECV_OPT="${CHIAKI_RECV_OPT:-1}" \
    -e CHIAKI_TRANSPORT_DIAG="${CHIAKI_TRANSPORT_DIAG:-0}" \
    -e BUILD_JOBS="${BUILD_JOBS:-}" \
    -v "$project_root:/work" -w /work \
    "$docker_image" \
    bash /work/tools/chiaki_switch/build_in_container.sh
