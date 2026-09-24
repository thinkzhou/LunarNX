#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
checkout="${CHIAKI_SOURCE_CHECKOUT:-$project_root/github_repos/chiaki-ng-fork}"
src=/tmp/lunarnx-chiaki-switch-src
build=/tmp/lunarnx-chiaki-switch-build
stage=/tmp/lunarnx-chiaki-switch-stage
tools_dir=/tmp/lunarnx-chiaki-tools
build_jobs="${BUILD_JOBS:-$(nproc)}"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
export PKG_CONFIG_PATH="$project_root/tools/pkgconfig"
expected_commit=907cd8219170b771a7d5d052fa8234b545b25a9f
export LUNARNX_CHIAKI_PBGEN="$project_root/tools/chiaki_switch/pbgen_v15"
# The earlier Takion and hole-punch patches do not apply to v15's rewrites.
# Keep the independently applicable Switch receive-capacity fixes.
patches=(
    lunarnx-chiaki-video-reorder-capacity.patch
    lunarnx-chiaki-recvbuf.patch
)

actual_commit="$(git -C "$checkout" rev-parse HEAD)"
if [[ "$actual_commit" != "$expected_commit" ]]; then
    echo "Expected chiaki-ng $expected_commit, found $actual_commit" >&2
    exit 1
fi
if [[ -n "$(git -C "$checkout" status --short)" ]]; then
    echo "Akira Chiaki checkout has local changes; refusing a non-reproducible build" >&2
    exit 1
fi

rm -rf "$src" "$build" "$stage" "$tools_dir"
mkdir -p "$src" "$stage/include" "$tools_dir"
cp -a "$checkout/." "$src/"
for patch in "${patches[@]}"; do
    git -C "$src" apply "$project_root/tools/chiaki_switch/$patch"
done

cp "$project_root/tools/chiaki_switch/protoc_from_pbgen.sh" "$tools_dir/protoc"
cp "$project_root/tools/chiaki_switch/pkg-config-wrapper.sh" "$tools_dir/pkg-config"
chmod +x "$tools_dir/protoc" "$tools_dir/pkg-config"
export PATH="$tools_dir:$PATH"

cmake -S "$src" -B "$build" \
    -DCMAKE_TOOLCHAIN_FILE="$src/cmake/switch.cmake" \
    -DPKG_CONFIG_EXECUTABLE="$tools_dir/pkg-config" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_C_FLAGS="-I$project_root/lib/switch/include -include $project_root/lib/switch/include/curl/curl.h" \
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
    -DCURL_LIBRARY="$project_root/lib/switch/libcurl.a" \
    -DCURL_INCLUDE_DIR="$project_root/lib/switch/include" \
    -DCHIAKI_USE_SYSTEM_NANOPB=OFF \
    -DCHIAKI_USE_SYSTEM_JERASURE=OFF \
    -DCHIAKI_LIB_JSONC_EXTERNAL_PROJECT=OFF \
    -DCHIAKI_LIB_MINIUPNPC_EXTERNAL_PROJECT=OFF
mkdir -p "$build/lib/protobuf"
cp "$LUNARNX_CHIAKI_PBGEN/takion.pb" "$build/lib/protobuf/takion.pb"
cp "$LUNARNX_CHIAKI_PBGEN/takion.pb.c" "$build/lib/protobuf/takion.pb.c"
cp "$LUNARNX_CHIAKI_PBGEN/takion.pb.h" "$build/lib/protobuf/takion.pb.h"
cmake --build "$build" --target chiaki-lib -j"$build_jobs"

ar="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar"
python3 - "$build/compile_commands.json" "$build/chiaki-abi.o" \
    "$project_root/tools/chiaki_switch/abi_library.c" <<'PY'
import json
import shlex
import subprocess
import sys
from pathlib import Path

compile_commands = json.loads(Path(sys.argv[1]).read_text())
output = sys.argv[2]
abi_source = sys.argv[3]
entry = next(item for item in compile_commands
             if item["file"].endswith("/lib/src/session.c"))
args = list(entry.get("arguments") or shlex.split(entry["command"]))
for index, argument in enumerate(args):
    if argument.endswith("/lib/src/session.c"):
        args[index] = abi_source
    if argument == "-o":
        args[index + 1] = output
subprocess.run(args, check=True)
PY
"$ar" rcs "$build/lib/libchiaki.a" "$build/chiaki-abi.o"

cp "$build/lib/libchiaki.a" "$stage/libchiaki.a"
copy_built_archive() {
    local archive_name="$1"
    local destination="$2"
    local archive_path
    archive_path="$(find "$build" -type f -name "$archive_name" -print -quit)"
    if [[ -z "$archive_path" ]]; then
        echo "Missing built Chiaki dependency: $archive_name" >&2
        exit 1
    fi
    cp "$archive_path" "$stage/$destination"
}
copy_built_archive libgf_complete.a libgf_complete.a
copy_built_archive libjerasure.a libjerasure.a
copy_built_archive libprotobuf-nanopb.a libprotobuf-nanopb.a
cp -R "$src/lib/include/chiaki" "$stage/include/"
mkdir -p "$stage/include/src/crypto/libnx"
cp "$src/lib/src/crypto/libnx/"*.h "$stage/include/src/crypto/libnx/"
cp "$build/lib/include/chiaki/config.h" "$stage/include/chiaki/config.h"

rm -rf "$project_root/lib/switch/include/chiaki" "$project_root/lib/switch/include/src"
mkdir -p "$project_root/lib/switch/include"
mv "$stage/include/chiaki" "$project_root/lib/switch/include/chiaki"
mv "$stage/include/src" "$project_root/lib/switch/include/src"
mv "$stage/libchiaki.a" "$project_root/lib/switch/libchiaki.a"
mv "$stage/libgf_complete.a" "$project_root/lib/switch/libgf_complete.a"
mv "$stage/libjerasure.a" "$project_root/lib/switch/libjerasure.a"
mv "$stage/libprotobuf-nanopb.a" "$project_root/lib/switch/libprotobuf-nanopb.a"
echo "Installed coherent Chiaki Switch SDK into $project_root/lib/switch"
