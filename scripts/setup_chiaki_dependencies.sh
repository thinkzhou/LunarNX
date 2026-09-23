#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
checkout="$project_root/github_repos/chiaki-ng-fork"
remote=https://github.com/xlanor/chiaki-ng.git
commit=907cd8219170b771a7d5d052fa8234b545b25a9f

if [[ ! -e "$checkout/.git" ]]; then
    mkdir -p "$(dirname "$checkout")"
    git clone --filter=blob:none --no-checkout "$remote" "$checkout"
    git -C "$checkout" checkout --detach "$commit"
fi

if [[ -n "$(git -C "$checkout" status --short)" ]]; then
    echo "Chiaki checkout has local changes: $checkout" >&2
    exit 1
fi
if [[ "$(git -C "$checkout" rev-parse HEAD)" != "$commit" ]]; then
    git -C "$checkout" fetch "$remote" "$commit"
    git -C "$checkout" checkout --detach "$commit"
fi

git -C "$checkout" submodule update --init --depth 1 \
    third-party/gf-complete \
    third-party/jerasure \
    third-party/nanopb
