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

    cp /work/github_repos/chiaki-ng/pbgen/takion.pb "$output"
