#!/usr/bin/env bash

set -euo pipefail

FFMPEG_COMMIT="e1094ac45d3bc7942043e72a23b6ab30faaddb8a"
WILIWILI_COMMIT="88e5876bea9502d06f46a8656e3530684d3aaf7d"
FFMPEG_SHA256="6d62556767127bbf49c3b7d7c6fa55f1223be6139c0de66707305368eaad05db"
FFMPEG_PATCH_SHA256="1792380b992e3554a4abcddf0d7b395bfd8c118ac7c6e38c8f2fb0d39753a390"
NETWORK_PATCH_SHA256="150c56eff36b1179f5409bf2e30fbf6996ce603ba6c365b6bf94928f116c97fa"

BUILD_ROOT="/work/build/ffmpeg-switch"
CACHE_DIR="$BUILD_ROOT/cache"
SOURCE_DIR="$BUILD_ROOT/FFmpeg-$FFMPEG_COMMIT"
PREFIX="$BUILD_ROOT/install"
LOCAL_PATCH="/work/tools/ffmpeg_switch_build/nvtegra-status-clear.patch"
LOCAL_PATCH_SHA256="$(sha256sum "$LOCAL_PATCH" | cut -d' ' -f1)"
BUILD_KEY="$FFMPEG_COMMIT:$WILIWILI_COMMIT:$FFMPEG_PATCH_SHA256:$NETWORK_PATCH_SHA256:$LOCAL_PATCH_SHA256:v1"
BUILD_KEY_FILE="$SOURCE_DIR/.lunarnx-build-key"

download() {
    local url="$1"
    local output="$2"
    local sha256="$3"

    if [[ -f "$output" ]] && echo "$sha256  $output" | sha256sum -c - >/dev/null 2>&1; then
        return
    fi

    rm -f "$output"
    curl -L --fail --retry 3 -o "$output" "$url"
    echo "$sha256  $output" | sha256sum -c -
}

if [[ "${DEVKITPRO:-}" != "/opt/devkitpro" ]]; then
    echo "This script must run inside the devkitPro devkitA64 container." >&2
    exit 1
fi

mkdir -p "$CACHE_DIR"
download \
    "https://github.com/FFmpeg/FFmpeg/archive/$FFMPEG_COMMIT.tar.gz" \
    "$CACHE_DIR/ffmpeg.tar.gz" \
    "$FFMPEG_SHA256"
download \
    "https://raw.githubusercontent.com/xfangfang/wiliwili/$WILIWILI_COMMIT/scripts/switch/ffmpeg/ffmpeg.patch" \
    "$CACHE_DIR/wiliwili-ffmpeg.patch" \
    "$FFMPEG_PATCH_SHA256"
download \
    "https://raw.githubusercontent.com/xfangfang/wiliwili/$WILIWILI_COMMIT/scripts/switch/ffmpeg/network.patch" \
    "$CACHE_DIR/wiliwili-network.patch" \
    "$NETWORK_PATCH_SHA256"

source /opt/devkitpro/switchvars.sh

if [[ ! -f "$BUILD_KEY_FILE" ]] || [[ "$(<"$BUILD_KEY_FILE")" != "$BUILD_KEY" ]]; then
    rm -rf "$SOURCE_DIR" "$PREFIX"
    tar -xzf "$CACHE_DIR/ffmpeg.tar.gz" -C "$BUILD_ROOT"

    cd "$SOURCE_DIR"
    rm -f \
        libavutil/hwcontext_nvtegra.c \
        libavutil/hwcontext_nvtegra.h \
        libavutil/nvdec_drv.h \
        libavutil/nvhost_ioctl.h \
        libavutil/nvjpg_drv.h \
        libavutil/nvmap_ioctl.h \
        libavutil/nvtegra.c \
        libavutil/nvtegra.h \
        libavutil/nvtegra_host1x.h \
        libavutil/vic_drv.h \
        libavutil/clb0b6.h \
        libavutil/clc5b0.h \
        libavutil/cle7d0.h \
        libavcodec/nvtegra_*

    patch -Np1 -i "$CACHE_DIR/wiliwili-ffmpeg.patch"
    patch -Np1 -i "$CACHE_DIR/wiliwili-network.patch"
    patch -Np1 -i "$LOCAL_PATCH"

    ./configure --prefix="$PREFIX" --enable-gpl --disable-shared --enable-static \
        --cross-prefix=aarch64-none-elf- --enable-cross-compile \
        --arch=aarch64 --cpu=cortex-a57 --target-os=horizon --enable-pic \
        --extra-cflags='-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec' \
        --extra-cxxflags='-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec' \
        --extra-ldflags='-fPIE -L/opt/devkitpro/portlibs/switch/lib -L/opt/devkitpro/libnx/lib' \
        --disable-runtime-cpudetect --disable-programs --disable-debug --disable-doc --disable-autodetect \
        --enable-asm --enable-neon \
        --disable-postproc --disable-avdevice --disable-encoders --disable-muxers \
        --enable-swscale --enable-swresample --enable-network \
        --disable-protocols --enable-protocol='file,http,tcp,udp,rtmp,hls,https,tls,ftp,rtp,crypto,httpproxy' \
        --enable-zlib --enable-bzlib --enable-libass --enable-libfreetype --enable-libfribidi --enable-libdav1d \
        --enable-nvtegra --enable-version3 --enable-mbedtls
    printf '%s\n' "$BUILD_KEY" > "$BUILD_KEY_FILE"
else
    cd "$SOURCE_DIR"
fi

make -j"${FFMPEG_JOBS:-$(nproc)}"
make install

echo "Switch FFmpeg installed to $PREFIX"
