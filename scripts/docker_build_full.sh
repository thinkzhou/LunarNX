#!/bin/bash
# Full LunarNX Switch build in Docker only.
# Uses devkitPro portlibs switch-curl by default, matching wiliwili's libnx TLS route.
# Libraries:
#   - Borealis: from source (CMake, same as Moonlight add_subdirectory)
#   - curl: devkitPro switch-curl with libnx ssl-service backend (wiliwili-style)
#   - FFmpeg: from Moonlight-Switch lib/switch/ for the current media pipeline
#   - mbedtls: legacy libpeer's pinned submodule, built from source in Docker
#   - srtp2: rebuilt in Docker for libpeer
#   - zstd/libnx/deko3d: from the devkitPro Docker image
set -euo pipefail

DOCKER_IMG="${DOCKER_IMG:-devkitpro/devkita64:20251117}"
DOCKER_PLATFORM="${DOCKER_PLATFORM:-linux/amd64}"
NETWORK_DIAG="${NETWORK_DIAG:-0}"
APP_DIAG="${APP_DIAG:-0}"
DROP_DIAG="${DROP_DIAG:-0}"
XBOX_RESPONSE_TRACE="${XBOX_RESPONSE_TRACE:-0}"
IPV6="${IPV6:-0}"
CURL_PROVIDER="${CURL_PROVIDER:-wiliwili}"
CURL_VERIFY="${CURL_VERIFY:-0}"
CURL_VERBOSE="${CURL_VERBOSE:-0}"
CURL_TIMEOUT_MS="${CURL_TIMEOUT_MS:-30000}"
APP_VERSION="${APP_VERSION:-0.1.0}"
LUNAR_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GIT_COMMIT="${GIT_COMMIT:-$(git -C "$LUNAR_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)}"
DEV_BRIDGE_UPLOAD_TOKEN="${DEV_BRIDGE_UPLOAD_TOKEN:-}"

if [[ ! -d "$LUNAR_DIR/lib/borealis/.git" || ! -d "$LUNAR_DIR/lib/libpeer/.git" ]]; then
    echo "Missing local dependencies. Run ./scripts/setup_dependencies.sh first." >&2
    exit 2
fi

if ! docker image inspect "$DOCKER_IMG" >/dev/null 2>&1; then
    echo "Docker image not found locally; Docker will pull $DOCKER_IMG."
fi

docker run --rm --platform "$DOCKER_PLATFORM" \
    -e NETWORK_DIAG="$NETWORK_DIAG" \
    -e APP_DIAG="$APP_DIAG" \
    -e DROP_DIAG="$DROP_DIAG" \
    -e XBOX_RESPONSE_TRACE="$XBOX_RESPONSE_TRACE" \
    -e IPV6="$IPV6" \
    -e CURL_PROVIDER="$CURL_PROVIDER" \
    -e CURL_VERIFY="$CURL_VERIFY" \
    -e CURL_VERBOSE="$CURL_VERBOSE" \
    -e CURL_TIMEOUT_MS="$CURL_TIMEOUT_MS" \
    -e APP_VERSION="$APP_VERSION" \
    -e GIT_COMMIT="$GIT_COMMIT" \
    -e DEV_BRIDGE_UPLOAD_TOKEN="$DEV_BRIDGE_UPLOAD_TOKEN" \
    -v "$LUNAR_DIR:/work" -w /work \
    "$DOCKER_IMG" bash -lc '
set -e
export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH

echo "Docker image: '"$DOCKER_IMG"'"
echo "NETWORK_DIAG: $NETWORK_DIAG"
echo "APP_DIAG: $APP_DIAG"
echo "DROP_DIAG: $DROP_DIAG"
echo "XBOX_RESPONSE_TRACE: $XBOX_RESPONSE_TRACE"
echo "IPV6: $IPV6"
echo "CURL_PROVIDER: $CURL_PROVIDER"
echo "CURL_VERIFY: $CURL_VERIFY"
echo "CURL_VERBOSE: $CURL_VERBOSE"
echo "CURL_TIMEOUT_MS: $CURL_TIMEOUT_MS"
echo "APP_VERSION: $APP_VERSION"
echo "GIT_COMMIT: $GIT_COMMIT"
aarch64-none-elf-g++ --version | head -1

case "$CURL_PROVIDER" in
    wiliwili|devkitpro)
        echo "========================================="
        echo " Step 0: Verify wiliwili switch-curl"
        echo "========================================="
        test -f /opt/devkitpro/portlibs/switch/lib/libcurl.a
        /opt/devkitpro/portlibs/switch/bin/curl-config --version
        curl_backend="$(/opt/devkitpro/portlibs/switch/bin/curl-config --ssl-backends)"
        echo "switch-curl ssl backend: $curl_backend"
        echo "$curl_backend" | grep -q libnx
        ;;
    moonlight)
        test -f /work/lib/switch/libcurl.a
        echo "Using Moonlight-Switch lib/switch/libcurl.a"
        ;;
    *)
        echo "Unsupported CURL_PROVIDER: $CURL_PROVIDER" >&2
        exit 2
        ;;
esac

echo "========================================="
echo " Step 1: Build Borealis (CMake, like Moonlight)"
echo "========================================="
cd /work/lib/borealis
git apply --reverse --check \
    /work/tools/borealis_switch/lunarnx-borealis-gpu-lifecycle.patch
rm -rf build_switch
cmake -S . -B build_switch \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
    -DPLATFORM_SWITCH=ON \
    -DUSE_DEKO3D=ON \
    -DBOREALIS_USE_DEKO3D=ON \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build_switch --target borealis -- -j$(nproc)
echo "  Borealis: OK"

echo ""
echo "========================================="
echo " Step 2: Build mbedtls and srtp2 for legacy libpeer"
echo "========================================="
cd /work/lib/libpeer/third_party/mbedtls
rm -rf build_switch
cmake -S . -B build_switch \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-DMBEDTLS_NO_PLATFORM_ENTROPY -DMBEDTLS_ENTROPY_HARDWARE_ALT" \
    -DENABLE_PROGRAMS=OFF \
    -DENABLE_TESTING=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON
cmake --build build_switch -- -j$(nproc)
echo "  mbedtls: OK"

cd /work/lib/libpeer/third_party/libsrtp
rm -rf build_switch
cmake -S . -B build_switch \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DTEST_APPS=OFF \
    -DBUILD_SHARED_LIBS=OFF
cmake --build build_switch -- -j$(nproc)
echo "  srtp2: OK"

echo ""
echo "========================================="
echo " Step 3: Build LunarNX (Makefile, with Moonlight libs)"
echo "========================================="
cd /work
make -f Makefile.switch clean
make -f Makefile.switch -j$(nproc) \
    NETWORK_DIAG="$NETWORK_DIAG" \
    APP_DIAG="$APP_DIAG" \
    DROP_DIAG="$DROP_DIAG" \
    XBOX_RESPONSE_TRACE="$XBOX_RESPONSE_TRACE" \
    IPV6="$IPV6" \
    CURL_PROVIDER="$CURL_PROVIDER" \
    CURL_VERIFY="$CURL_VERIFY" \
    CURL_VERBOSE="$CURL_VERBOSE" \
    CURL_TIMEOUT_MS="$CURL_TIMEOUT_MS" \
    APP_VERSION="$APP_VERSION" \
    GIT_COMMIT="$GIT_COMMIT" \
    DEV_BRIDGE_UPLOAD_TOKEN="$DEV_BRIDGE_UPLOAD_TOKEN"

echo ""
echo "========================================="
echo " Done!"
echo "========================================="
ls -lh /work/build/switch/LunarNX.nro
aarch64-none-elf-nm /work/build/switch/LunarNX.elf | grep -E "__appInit|userAppInit"
aarch64-none-elf-readelf -S /work/build/switch/LunarNX.elf | grep -E "rel[ar]|init_array|fini_array"
'
