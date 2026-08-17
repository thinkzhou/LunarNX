#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
checkout="$project_root/github_repos/chiaki-ng-fork"
remote=https://github.com/xlanor/chiaki-ng.git
commit=1597a48514e5d9e67168ca40e6fa40c0171cd379

if [[ ! -d "$checkout/.git" ]]; then
    mkdir -p "$(dirname "$checkout")"
    git clone --filter=blob:none --no-checkout "$remote" "$checkout"
    git -C "$checkout" checkout --detach "$commit"
fi

if [[ "$(git -C "$checkout" rev-parse HEAD)" != "$commit" ]]; then
    echo "Expected chiaki-ng $commit in $checkout" >&2
    exit 1
fi
if [[ -n "$(git -C "$checkout" status --short)" ]]; then
    echo "Chiaki checkout has local changes: $checkout" >&2
    exit 1
fi

git -C "$checkout" submodule update --init --depth 1 \
    third-party/gf-complete \
    third-party/jerasure \
    third-party/nanopb
