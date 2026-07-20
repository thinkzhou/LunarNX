#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${IMAGE:-devkitpro/devkita64:20251117}"
PROXY_URL="${PROXY_URL:-}"

docker_env=()
if [[ -n "$PROXY_URL" ]]; then
  docker_env+=(
    -e "http_proxy=${PROXY_URL}"
    -e "https_proxy=${PROXY_URL}"
    -e "HTTP_PROXY=${PROXY_URL}"
    -e "HTTPS_PROXY=${PROXY_URL}"
  )
fi

docker run --rm --platform linux/amd64 \
  "${docker_env[@]}" \
  -v "${ROOT_DIR}:/work" \
  -w /work \
  "${IMAGE}" \
  bash -lc ./tools/libdatachannel_switch_spike/run_inside_docker.sh
