#!/usr/bin/env bash

# Canonical Switch build entry point for both local Docker builds and the
# GitHub Actions devkitPro job container.
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"

network_diag="${NETWORK_DIAG:-0}"
app_diag="${APP_DIAG:-0}"
drop_diag="${DROP_DIAG:-0}"
xbox_response_trace="${XBOX_RESPONSE_TRACE:-0}"
ipv6="${IPV6:-0}"
curl_provider="${CURL_PROVIDER:-moonlight}"
curl_verify="${CURL_VERIFY:-0}"
curl_verbose="${CURL_VERBOSE:-0}"
curl_timeout_ms="${CURL_TIMEOUT_MS:-30000}"
app_version="${APP_VERSION:-0.2.0}"
git_commit="${GIT_COMMIT:-$(git -C "$project_root" rev-parse --short HEAD 2>/dev/null || echo unknown)}"
dev_bridge_upload_token="${DEV_BRIDGE_UPLOAD_TOKEN:-}"
build_jobs="${BUILD_JOBS:-$(nproc)}"

if [[ ! -d "$project_root/lib/borealis/.git" ||
      ! -d "$project_root/lib/libpeer/.git" ]]; then
    echo "Missing local dependencies. Run ./scripts/setup_dependencies.sh first." >&2
    exit 2
fi

echo "DEVKITPRO: $DEVKITPRO"
echo "NETWORK_DIAG: $network_diag"
echo "APP_DIAG: $app_diag"
echo "DROP_DIAG: $drop_diag"
echo "XBOX_RESPONSE_TRACE: $xbox_response_trace"
echo "IPV6: $ipv6"
echo "CURL_PROVIDER: $curl_provider"
echo "CURL_VERIFY: $curl_verify"
echo "CURL_VERBOSE: $curl_verbose"
echo "CURL_TIMEOUT_MS: $curl_timeout_ms"
echo "APP_VERSION: $app_version"
echo "GIT_COMMIT: $git_commit"
echo "BUILD_JOBS: $build_jobs"
aarch64-none-elf-g++ --version | head -1

case "$curl_provider" in
    wiliwili|devkitpro)
        test -f "$DEVKITPRO/portlibs/switch/lib/libcurl.a"
        "$DEVKITPRO/portlibs/switch/bin/curl-config" --version
        curl_backend="$("$DEVKITPRO/portlibs/switch/bin/curl-config" --ssl-backends)"
        echo "switch-curl ssl backend: $curl_backend"
        grep -q libnx <<<"$curl_backend"
        ;;
    moonlight)
        test -f "$project_root/lib/switch/libcurl.a"
        echo "Using Moonlight-Switch lib/switch/libcurl.a"
        ;;
    *)
        echo "Unsupported CURL_PROVIDER: $curl_provider" >&2
        exit 2
        ;;
esac

echo "Building Borealis"
git -C "$project_root/lib/borealis" apply --reverse --check \
    "$project_root/tools/borealis_switch/lunarnx-borealis-gpu-lifecycle.patch"
rm -rf "$project_root/lib/borealis/build_switch"
cmake -S "$project_root/lib/borealis" \
    -B "$project_root/lib/borealis/build_switch" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DPLATFORM_SWITCH=ON \
    -DUSE_DEKO3D=ON \
    -DBOREALIS_USE_DEKO3D=ON \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$project_root/lib/borealis/build_switch" \
    --target borealis -- -j"$build_jobs"

echo "Building mbedTLS"
mbedtls_dir="$project_root/lib/libpeer/third_party/mbedtls"
rm -rf "$mbedtls_dir/build_switch"
cmake -S "$mbedtls_dir" -B "$mbedtls_dir/build_switch" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-DMBEDTLS_NO_PLATFORM_ENTROPY -DMBEDTLS_ENTROPY_HARDWARE_ALT" \
    -DENABLE_PROGRAMS=OFF \
    -DENABLE_TESTING=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON
cmake --build "$mbedtls_dir/build_switch" -- -j"$build_jobs"

echo "Building libsrtp"
libsrtp_dir="$project_root/lib/libpeer/third_party/libsrtp"
rm -rf "$libsrtp_dir/build_switch"
cmake -S "$libsrtp_dir" -B "$libsrtp_dir/build_switch" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTEST_APPS=OFF \
    -DBUILD_SHARED_LIBS=OFF
cmake --build "$libsrtp_dir/build_switch" -- -j"$build_jobs"

echo "Building LunarNX"
make -C "$project_root" -f Makefile.switch clean
make -C "$project_root" -f Makefile.switch -j"$build_jobs" \
    NETWORK_DIAG="$network_diag" \
    APP_DIAG="$app_diag" \
    DROP_DIAG="$drop_diag" \
    XBOX_RESPONSE_TRACE="$xbox_response_trace" \
    IPV6="$ipv6" \
    CURL_PROVIDER="$curl_provider" \
    CURL_VERIFY="$curl_verify" \
    CURL_VERBOSE="$curl_verbose" \
    CURL_TIMEOUT_MS="$curl_timeout_ms" \
    APP_VERSION="$app_version" \
    GIT_COMMIT="$git_commit" \
    DEV_BRIDGE_UPLOAD_TOKEN="$dev_bridge_upload_token"

ls -lh "$project_root/build/switch/LunarNX.nro"
aarch64-none-elf-nm "$project_root/build/switch/LunarNX.elf" | \
    grep -E "__appInit|userAppInit"
aarch64-none-elf-readelf -S "$project_root/build/switch/LunarNX.elf" | \
    grep -E "rel[ar]|init_array|fini_array"
