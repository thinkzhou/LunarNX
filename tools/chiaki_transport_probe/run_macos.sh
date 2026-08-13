#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CHIAKI="$ROOT/github_repos/chiaki-ng-fork"
HOST_BUILD="$ROOT/github_repos/chiaki-ng/build_macos"
OUT="${TMPDIR:-/tmp}/lunarnx-chiaki-transport-probe"

test "$(git -C "$CHIAKI" rev-parse HEAD)" = "1597a48514e5d9e67168ca40e6fa40c0171cd379"
mkdir -p "$OUT"

INCLUDES="-I$CHIAKI/lib/include -I$HOST_BUILD/lib/include -I$CHIAKI/third-party/jerasure/include -I$CHIAKI/third-party/gf-complete/include"
for source in reorderqueue frameprocessor fec log; do
    clang -O2 -DNDEBUG $INCLUDES -c "$CHIAKI/lib/src/$source.c" -o "$OUT/$source.o"
done
clang -O2 -DNDEBUG $INCLUDES \
    -c "$ROOT/tools/chiaki_transport_probe/packetstats_stub.c" \
    -o "$OUT/packetstats_stub.o"
clang -O2 -DNDEBUG $INCLUDES \
    "$ROOT/tools/chiaki_transport_probe/chiaki_transport_probe.c" \
    "$OUT/reorderqueue.o" "$OUT/frameprocessor.o" "$OUT/fec.o" "$OUT/log.o" \
    "$OUT/packetstats_stub.o" \
    "$HOST_BUILD/third-party/libjerasure.a" \
    "$HOST_BUILD/third-party/libgf_complete.a" \
    -o "$OUT/chiaki_transport_probe"

"$OUT/chiaki_transport_probe"
