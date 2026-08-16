#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
IMAGE=${LUNARNX_DEVKIT_IMAGE:-devkitpro/devkita64:20251117}
CHIAKI_RECV_OPT=${CHIAKI_RECV_OPT:-1}
CHIAKI_TRANSPORT_DIAG=${CHIAKI_TRANSPORT_DIAG:-0}

case "$CHIAKI_RECV_OPT" in
    0|1) ;;
    *) echo "CHIAKI_RECV_OPT must be 0 or 1" >&2; exit 2 ;;
esac
case "$CHIAKI_TRANSPORT_DIAG" in
    0|1) ;;
    *) echo "CHIAKI_TRANSPORT_DIAG must be 0 or 1" >&2; exit 2 ;;
esac

docker run --rm --platform linux/amd64 \
    -e CHIAKI_RECV_OPT="$CHIAKI_RECV_OPT" \
    -e CHIAKI_TRANSPORT_DIAG="$CHIAKI_TRANSPORT_DIAG" \
    -v "$ROOT:/work" -w /work "$IMAGE" bash -lc '
set -euo pipefail
export DEVKITPRO=/opt/devkitpro
export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
export PKG_CONFIG_PATH=/work/tools/pkgconfig

checkout=/work/github_repos/chiaki-ng-fork
src=/tmp/lunarnx-chiaki-switch-src
build=/tmp/lunarnx-chiaki-switch-build
stage=/tmp/lunarnx-chiaki-switch-stage
expected_commit=1597a48514e5d9e67168ca40e6fa40c0171cd379
actual_commit=$(git -C "$checkout" rev-parse HEAD)
if [ "$actual_commit" != "$expected_commit" ]; then
    echo "Expected chiaki-ng $expected_commit, found $actual_commit" >&2
    exit 1
fi
if [ -n "$(git -C "$checkout" status --short)" ]; then
    echo "Akira Chiaki checkout has local changes; refusing a non-reproducible build" >&2
    exit 1
fi
rm -rf "$src" "$build" "$stage"
mkdir -p "$src"
cp -a "$checkout/." "$src/"
git -C "$src" apply /work/tools/chiaki_switch/lunarnx-chiaki-stun-order.patch
git -C "$src" apply /work/tools/chiaki_switch/lunarnx-chiaki-stream-switch.patch
git -C "$src" apply /work/tools/chiaki_switch/lunarnx-chiaki-video-reorder-capacity.patch
git -C "$src" apply /work/tools/chiaki_switch/lunarnx-chiaki-recv-allocation.patch
git -C "$src" apply /work/tools/chiaki_switch/lunarnx-chiaki-holepunch-reliability.patch
git -C "$src" apply /work/tools/chiaki_switch/lunarnx-chiaki-stream-rtt.patch
git -C "$src" apply /work/tools/chiaki_switch/lunarnx-chiaki-recvbuf.patch
git -C "$src" apply /work/tools/chiaki_switch/lunarnx-chiaki-packetstats-wrap.patch
git -C "$src" apply --recount /work/tools/chiaki_switch/lunarnx-chiaki-transport-diagnostics.patch
git -C "$src" apply --recount /work/tools/chiaki_switch/lunarnx-chiaki-key-position-diagnostics.patch
mkdir -p "$stage/include"
mkdir -p /tmp/lunarnx-chiaki-tools
cp /work/tools/chiaki_switch/protoc_from_pbgen.sh /tmp/lunarnx-chiaki-tools/protoc
cp /work/tools/chiaki_switch/pkg-config-wrapper.sh /tmp/lunarnx-chiaki-tools/pkg-config
chmod +x /tmp/lunarnx-chiaki-tools/protoc
chmod +x /tmp/lunarnx-chiaki-tools/pkg-config
export PATH=/tmp/lunarnx-chiaki-tools:$PATH

cmake -S "$src" -B "$build" \
    -DCMAKE_TOOLCHAIN_FILE="$src/cmake/switch.cmake" \
    -DPKG_CONFIG_EXECUTABLE=/tmp/lunarnx-chiaki-tools/pkg-config \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_C_FLAGS="-I/work/lib/switch/include/curl -include /work/lib/switch/include/curl/curl/curl.h -DLUNARNX_CHIAKI_RECV_OPT=$CHIAKI_RECV_OPT -DLUNARNX_CHIAKI_TRANSPORT_DIAG=$CHIAKI_TRANSPORT_DIAG" \
    -DNSWITCH=TRUE \
    -DCHIAKI_ENABLE_TESTS=OFF \
    -DCHIAKI_ENABLE_CLI=OFF \
    -DCHIAKI_ENABLE_GUI=OFF \
    -DCHIAKI_ENABLE_BOREALIS=OFF \
    -DCHIAKI_ENABLE_FFMPEG_DECODER=OFF \
    -DCHIAKI_ENABLE_PI_DECODER=OFF \
    -DCHIAKI_ENABLE_SETSU=OFF \
    -DCHIAKI_ENABLE_SPEEX=OFF \
    -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF \
    -DCHIAKI_ENABLE_STEAM_SHORTCUT=OFF \
    -DCHIAKI_ENABLE_RUDP=ON \
    -DCHIAKI_LIB_ENABLE_OPUS=ON \
    -DCHIAKI_LIB_ENABLE_LIBNX_CRYPTO=ON \
    -DCHIAKI_LIB_ENABLE_LIBNX_EXPERIMENTAL=ON \
    -DCHIAKI_USE_SYSTEM_CURL=ON \
    -DCURL_LIBRARY=/work/lib/switch/libcurl.a \
    -DCURL_INCLUDE_DIR=/work/lib/switch/include/curl \
    -DCHIAKI_USE_SYSTEM_NANOPB=OFF \
    -DCHIAKI_USE_SYSTEM_JERASURE=OFF \
    -DCHIAKI_LIB_JSONC_EXTERNAL_PROJECT=OFF \
    -DCHIAKI_LIB_MINIUPNPC_EXTERNAL_PROJECT=OFF
mkdir -p "$build/lib/protobuf"
cp /work/github_repos/chiaki-ng/pbgen/takion.pb "$build/lib/protobuf/takion.pb"
cp /work/github_repos/chiaki-ng/pbgen/takion.pb.c "$build/lib/protobuf/takion.pb.c"
cp /work/github_repos/chiaki-ng/pbgen/takion.pb.h "$build/lib/protobuf/takion.pb.h"
cmake --build "$build" --target chiaki-lib -j"$(nproc)"

ar=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar
python3 - "$build/compile_commands.json" "$build/chiaki-abi.o" <<'PY'
import json
import subprocess
import sys
from pathlib import Path
import shlex

compile_commands = json.loads(Path(sys.argv[1]).read_text())
output = sys.argv[2]
entry = next(item for item in compile_commands
             if item["file"].endswith("/lib/src/session.c"))
args = entry.get("arguments") or shlex.split(entry["command"])
args = list(args)
for index, argument in enumerate(args):
    if argument.endswith("/lib/src/session.c"):
        args[index] = "/work/tools/chiaki_switch/abi_library.c"
    if argument == "-o":
        args[index + 1] = output
subprocess.run(args, check=True)
PY
"$ar" rcs "$build/lib/libchiaki.a" "$build/chiaki-abi.o"

cp "$build/lib/libchiaki.a" "$stage/libchiaki.a"
cp -R "$src/lib/include/chiaki" "$stage/include/"
# The Akira public gkcrypt.h intentionally refers to the libnx backend with a
# relative ../src path.  Preserve that source-tree-relative include layout in
# the SDK staging directory; this packages the original headers unchanged and
# avoids requiring consumers to include the external checkout source tree.
mkdir -p "$stage/include/src/crypto/libnx"
cp "$src/lib/src/crypto/libnx/"*.h "$stage/include/src/crypto/libnx/"
cp "$build/lib/include/chiaki/config.h" "$stage/include/chiaki/config.h"
rm -rf /work/lib/switch/include/chiaki /work/lib/switch/include/src
mkdir -p /work/lib/switch/include
mv "$stage/include/chiaki" /work/lib/switch/include/chiaki
mv "$stage/include/src" /work/lib/switch/include/src
mv "$stage/libchiaki.a" /work/lib/switch/libchiaki.a
echo "Installed coherent Chiaki Switch SDK into /work/lib/switch"
'
