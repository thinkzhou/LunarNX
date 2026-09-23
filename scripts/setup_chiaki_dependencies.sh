#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
remote=https://github.com/xlanor/chiaki-ng.git
case "${CHIAKI_SDK_PROFILE:-legacy}" in
    legacy)
        checkout="$project_root/github_repos/chiaki-ng-fork"
        commit=1597a48514e5d9e67168ca40e6fa40c0171cd379
        ;;
    akira-v15)
        checkout="$project_root/github_repos/chiaki-ng-fork-akira-v15"
        commit=907cd8219170b771a7d5d052fa8234b545b25a9f
        ;;
    *) echo "CHIAKI_SDK_PROFILE must be legacy or akira-v15" >&2; exit 2 ;;
esac

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
    if [[ "${CHIAKI_SDK_PROFILE:-legacy}" == legacy ]]; then
        echo "Expected chiaki-ng $commit in $checkout" >&2
        exit 1
    fi
    git -C "$checkout" fetch "$remote" "$commit"
    git -C "$checkout" checkout --detach "$commit"
fi

git -C "$checkout" submodule update --init --depth 1 \
    third-party/gf-complete \
    third-party/jerasure \
    third-party/nanopb
