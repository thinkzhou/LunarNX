#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
borealis_dir="$project_root/lib/borealis"
libpeer_dir="$project_root/lib/libpeer"
borealis_patch="$project_root/tools/borealis_switch/lunarnx-borealis-gpu-lifecycle.patch"
borealis_command_buffer_patch="$project_root/tools/borealis_switch/lunarnx-borealis-command-buffer.patch"
libpeer_patch_dir="$project_root/tools/libpeer_legacy"
libpeer_nested_patch_dir="$libpeer_patch_dir/nested"

borealis_commit="240223f372731949e04cba943c453dc45b69faa1"
libpeer_commit="9319aa434cb9e893faed0293ba9d2a21eca59c8b"

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

if git -C "$borealis_dir" apply --reverse --check "$borealis_patch" >/dev/null 2>&1; then
    printf 'LunarNX Borealis GPU lifecycle patch is already applied.\n'
elif git -C "$borealis_dir" apply --check "$borealis_patch" >/dev/null 2>&1; then
    git -C "$borealis_dir" apply "$borealis_patch"
    printf 'Applied LunarNX Borealis GPU lifecycle patch.\n'
else
    printf 'Borealis GPU lifecycle patch conflicts with the local checkout: %s\n' \
        "$borealis_dir" >&2
    exit 2
fi

if git -C "$borealis_dir" apply --reverse --check "$borealis_command_buffer_patch" >/dev/null 2>&1; then
    printf 'LunarNX Borealis command-buffer patch is already applied.\n'
elif git -C "$borealis_dir" apply --check "$borealis_command_buffer_patch" >/dev/null 2>&1; then
    git -C "$borealis_dir" apply "$borealis_command_buffer_patch"
    printf 'Applied LunarNX Borealis command-buffer patch.\n'
else
    printf 'Borealis command-buffer patch conflicts with the local checkout: %s\n' \
        "$borealis_dir" >&2
    exit 2
fi

clone_at_commit \
    https://github.com/sepfy/libpeer.git \
    "$libpeer_dir" \
    "$libpeer_commit"

for libpeer_patch in \
    "$libpeer_patch_dir/0001-switch-adapt-libpeer-WebRTC-path.patch" \
    "$libpeer_patch_dir/0002-fix-H264-access-unit-flush-and-quiet-SCTP-logs.patch" \
    "$libpeer_patch_dir/legacy-libpeer-switch.patch" \
    "$libpeer_patch_dir/0003-enable-usrsctp-datachannel-policy.patch"
do
    if git -C "$libpeer_dir" apply --reverse --check "$libpeer_patch" >/dev/null 2>&1; then
        printf 'LunarNX libpeer patch is already applied: %s\n' "$(basename "$libpeer_patch")"
    elif git -C "$libpeer_dir" apply --check "$libpeer_patch" >/dev/null 2>&1; then
        git -C "$libpeer_dir" apply --index "$libpeer_patch"
        printf 'Applied LunarNX libpeer patch: %s\n' "$(basename "$libpeer_patch")"
    else
        printf 'libpeer patch conflicts with the local checkout: %s\n' \
            "$libpeer_patch" >&2
        exit 2
    fi
done

apply_nested_patch() {
    local submodule="$1"
    local patch="$2"
    local submodule_dir="$libpeer_dir/third_party/$submodule"

    if git -C "$submodule_dir" apply --reverse --check "$patch" >/dev/null 2>&1; then
        printf 'LunarNX nested libpeer patch is already applied: %s\n' "$(basename "$patch")"
    elif git -C "$submodule_dir" apply --check "$patch" >/dev/null 2>&1; then
        git -C "$submodule_dir" apply "$patch"
        printf 'Applied LunarNX nested libpeer patch: %s\n' "$(basename "$patch")"
    else
        printf 'Nested libpeer patch conflicts with %s: %s\n' "$submodule" "$patch" >&2
        exit 2
    fi
}

apply_nested_patch libsrtp \
    "$libpeer_nested_patch_dir/0001-switch-add-libnx-network-byte-order-includes.patch"
apply_nested_patch mbedtls \
    "$libpeer_nested_patch_dir/0001-switch-configure-mbedtls-for-DTLS-SRTP.patch"
apply_nested_patch usrsctp \
    "$libpeer_nested_patch_dir/0001-switch-add-libnx-compatibility-stubs.patch"

git -C "$libpeer_dir" submodule update --init --recursive

printf 'Dependency setup complete.\n'
