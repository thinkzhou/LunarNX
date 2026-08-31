#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CHIAKI="${LUNARNX_CHIAKI_CHECKOUT:-$ROOT/github_repos/chiaki-ng-fork}"
OUT="${TMPDIR:-/tmp}/lunarnx-chiaki-registration-probe"
EXPECTED_COMMIT=1597a48514e5d9e67168ca40e6fa40c0171cd379

test "$(git -C "$CHIAKI" rev-parse HEAD)" = "$EXPECTED_COMMIT"
mkdir -p "$OUT"

INCLUDES="-I$CHIAKI/lib/include -I/opt/homebrew/include"
clang -O2 $INCLUDES \
    "$ROOT/tools/chiaki_registration_probe/chiaki_registration_probe.c" \
    "$CHIAKI/lib/src/regist.c" \
    "$CHIAKI/lib/src/base64.c" \
    "$CHIAKI/lib/src/http.c" \
    "$CHIAKI/lib/src/log.c" \
    "$CHIAKI/lib/src/random.c" \
    "$CHIAKI/lib/src/rpcrypt.c" \
    "$CHIAKI/lib/src/sock.c" \
    "$CHIAKI/lib/src/stoppipe.c" \
    "$CHIAKI/lib/src/thread.c" \
    "$CHIAKI/lib/src/time.c" \
    -L/opt/homebrew/lib -lcrypto -lpthread \
    -o "$OUT/chiaki_registration_probe"

"$OUT/chiaki_registration_probe"
