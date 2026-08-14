#!/bin/zsh

set -eu

SCRIPT_DIR=${0:A:h}
OUTPUT_DIR=${1:-$SCRIPT_DIR/inbox}
mkdir -p "$OUTPUT_DIR"

if [[ -z "${LUNARNX_DEV_BRIDGE_URL:-}" ]]; then
    print -u2 -- 'Set LUNARNX_DEV_BRIDGE_URL to the deployed Worker URL'
    exit 2
fi
if [[ -z "${LUNARNX_DEV_BRIDGE_ADMIN_TOKEN:-}" ]]; then
    print -u2 -- 'Set LUNARNX_DEV_BRIDGE_ADMIN_TOKEN'
    exit 3
fi

NAME="lunarnx-$(date -u +%Y%m%d-%H%M%S).log"
/usr/bin/curl --fail --silent --show-error --location \
    -H "Authorization: Bearer $LUNARNX_DEV_BRIDGE_ADMIN_TOKEN" \
    -o "$OUTPUT_DIR/$NAME" \
    "${LUNARNX_DEV_BRIDGE_URL%/}/admin/logs/latest"
print -- "$OUTPUT_DIR/$NAME"
