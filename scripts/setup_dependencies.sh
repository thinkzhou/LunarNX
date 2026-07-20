#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
borealis_dir="$project_root/lib/borealis"
libpeer_dir="$project_root/lib/libpeer"
libpeer_patch="$project_root/tools/libpeer_legacy/legacy-libpeer-switch.patch"

borealis_commit="240223f372731949e04cba943c453dc45b69faa1"
libpeer_commit="bdc50f0cae13f19a31bb11827daea3a8354b173f"

clone_at_commit() {
    local url="$1"
    local destination="$2"
    local commit="$3"

    if [[ -e "$destination" ]]; then
        if [[ ! -d "$destination/.git" ]]; then
            printf 'Dependency path exists but is not a Git checkout: %s\n' "$destination" >&2
            exit 2
        fi
        local current_commit
        current_commit="$(git -C "$destination" rev-parse HEAD)"
        if [[ "$current_commit" != "$commit" ]]; then
            printf 'Dependency has unexpected revision: %s\nexpected: %s\nactual:   %s\n' \
                "$destination" "$commit" "$current_commit" >&2
            exit 2
        fi
        git -C "$destination" submodule update --init --recursive
        printf 'Dependency already exists at the pinned revision: %s\n' "$destination"
        return
    fi

    git clone "$url" "$destination"
    git -C "$destination" checkout "$commit"
    git -C "$destination" submodule update --init --recursive
}

clone_at_commit \
    https://github.com/XITRIX/borealis.git \
    "$borealis_dir" \
    "$borealis_commit"

libpeer_was_missing=0
if [[ ! -e "$libpeer_dir" ]]; then
    libpeer_was_missing=1
fi
clone_at_commit \
    https://github.com/sepfy/libpeer.git \
    "$libpeer_dir" \
    "$libpeer_commit"

if [[ "$libpeer_was_missing" -eq 1 ]]; then
    git -C "$libpeer_dir" apply "$libpeer_patch"
    printf 'Applied LunarNX legacy libpeer patch.\n'
elif git -C "$libpeer_dir" apply --reverse --check "$libpeer_patch" >/dev/null 2>&1; then
    printf 'LunarNX legacy libpeer patch is already applied.\n'
else
    printf '%s\n' \
        'Existing lib/libpeer was not modified.' \
        'Verify its revision and apply tools/libpeer_legacy/legacy-libpeer-switch.patch manually.'
fi

printf 'Dependency setup complete.\n'
