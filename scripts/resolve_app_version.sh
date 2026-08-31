#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version_file="${VERSION_FILE:-$project_root/version.txt}"
release_tag="${RELEASE_TAG:-}"
commit_sha="${GIT_COMMIT_SHA:-$(git -C "$project_root" rev-parse HEAD 2>/dev/null || true)}"

base_version="$(tr -d '[:space:]' < "$version_file")"
if [[ ! "$base_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "version.txt must contain MAJOR.MINOR.PATCH" >&2
    exit 2
fi

if [[ -n "$release_tag" ]]; then
    if [[ ! "$release_tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "Release tags must use vMAJOR.MINOR.PATCH" >&2
        exit 2
    fi
    release_version="${release_tag#v}"
    if [[ "$release_version" != "$base_version" ]]; then
        echo "Release tag $release_tag does not match version.txt ($base_version)" >&2
        exit 2
    fi
    printf '%s\n' "$release_version"
    exit 0
fi

if [[ ! "$commit_sha" =~ ^[0-9a-fA-F]{7,}$ ]]; then
    echo "GIT_COMMIT_SHA must contain at least seven hexadecimal characters" >&2
    exit 2
fi

printf '%s-g%s\n' "$base_version" "${commit_sha:0:7}"
