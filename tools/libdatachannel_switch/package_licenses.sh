#!/usr/bin/env bash
# Include these notices even in a feature-off package, so downgrade packages
# carry the same complete dependency inventory and corresponding-source guide.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DESTINATION="${1:?usage: package_licenses.sh artifact-directory}"
mkdir -p "$DESTINATION/licenses" "$DESTINATION/sources"
cp "$ROOT/LICENSES/libdatachannel-libjuice-MPL-2.0.txt" \
   "$ROOT/LICENSES/plog-MIT.txt" "$ROOT/LICENSES/usrsctp-BSD-3-Clause.txt" \
   "$DESTINATION/licenses/"
cp "$ROOT/tools/libdatachannel_switch/README.md" "$DESTINATION/sources/WebRTC-SDK.md"
cp "$ROOT/tools/libdatachannel_switch/prepare.py" "$DESTINATION/sources/prepare-webrtc.py"

REVISION="${GITHUB_SHA:-$(git -C "$ROOT" rev-parse HEAD)}"
printf '%s\n' "$REVISION" "https://github.com/thinkzhou/LunarNX/tree/$REVISION" \
    > "$DESTINATION/sources/LunarNX-revision.txt"
