#!/usr/bin/env bash
set -euo pipefail

# Optional checkout path also allows testing a fresh clone without disturbing
# the developer's existing dependency trees. Revisions come from Git gitlinks.
project_root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
git -C "$project_root" submodule update --init --recursive -- \
    vendor/ihslib vendor/protobuf-c

for header in vendor/ihslib/include/ihslib/client.h vendor/protobuf-c/protobuf-c/protobuf-c.h; do
    if [[ ! -f "$project_root/$header" ]]; then
        printf 'Steam dependency setup did not produce required header: %s\n' "$header" >&2
        exit 2
    fi
done
printf 'Pinned Steam dependencies are ready.\n'
