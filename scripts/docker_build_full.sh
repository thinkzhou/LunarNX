#!/usr/bin/env bash

# Run the canonical Switch build inside the pinned devkitPro image. GitHub
# Actions calls build_switch_in_container.sh directly from the same image.
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
docker_image="${DOCKER_IMG:-devkitpro/devkita64:20251117}"
docker_platform="${DOCKER_PLATFORM:-linux/amd64}"

if [[ ! -d "$project_root/lib/borealis/.git" ||
      ! -d "$project_root/lib/libpeer/.git" ]]; then
    echo "Missing local dependencies. Run ./scripts/setup_dependencies.sh first." >&2
    exit 2
fi

if ! docker image inspect "$docker_image" >/dev/null 2>&1; then
    echo "Docker image not found locally; Docker will pull $docker_image."
fi

docker run --rm --platform "$docker_platform" \
    -e NETWORK_DIAG="${NETWORK_DIAG:-0}" \
    -e APP_DIAG="${APP_DIAG:-0}" \
    -e DROP_DIAG="${DROP_DIAG:-0}" \
    -e XBOX_RESPONSE_TRACE="${XBOX_RESPONSE_TRACE:-0}" \
    -e IPV6="${IPV6:-0}" \
    -e CURL_PROVIDER="${CURL_PROVIDER:-moonlight}" \
    -e CURL_VERIFY="${CURL_VERIFY:-0}" \
    -e CURL_VERBOSE="${CURL_VERBOSE:-0}" \
    -e CURL_TIMEOUT_MS="${CURL_TIMEOUT_MS:-30000}" \
    -e APP_VERSION="${APP_VERSION:-0.2.0}" \
    -e GIT_COMMIT="${GIT_COMMIT:-$(git -C "$project_root" rev-parse --short HEAD 2>/dev/null || echo unknown)}" \
    -e DEV_BRIDGE_UPLOAD_TOKEN="${DEV_BRIDGE_UPLOAD_TOKEN:-}" \
    -e BUILD_JOBS="${BUILD_JOBS:-}" \
    -v "$project_root:/work" -w /work \
    "$docker_image" \
    bash /work/scripts/build_switch_in_container.sh
