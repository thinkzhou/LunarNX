#!/bin/sh
set -eu

output=
for argument in "$@"; do
    case "$argument" in
        -o*) output=${argument#-o} ;;
    esac
done

if [ -z "$output" ]; then
    echo "protoc_from_pbgen: expected -o<descriptor>" >&2
    exit 2
fi

if [ -z "${LUNARNX_CHIAKI_PBGEN:-}" ]; then
    echo "protoc_from_pbgen: LUNARNX_CHIAKI_PBGEN is not set" >&2
    exit 2
fi

cp "$LUNARNX_CHIAKI_PBGEN/takion.pb" "$output"
