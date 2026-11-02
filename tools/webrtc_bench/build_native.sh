#!/usr/bin/env bash
# Desktop-only build: this script never produces Switch-targeted artifacts.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
if [[ "$(uname -s)" != Darwin ]]; then
  echo 'This initial benchmark recipe targets macOS. Use Docker for Switch builds.' >&2
  exit 1
fi
PEER="$ROOT/lib/libpeer"
for archive in "$PEER/third_party/mbedtls/build_host/library/libmbedtls.a" \
  "$PEER/third_party/mbedtls/build_host/library/libmbedcrypto.a" \
  "$PEER/third_party/mbedtls/build_host/library/libmbedx509.a" \
  "$PEER/third_party/libsrtp/build_host/libsrtp2.a"; do
  [[ -f "$archive" ]] || { echo "Missing native prerequisite: $archive" >&2; exit 1; }
done
# Both receivers and the sender deliberately link the very same non-GCM libSRTP.
# This avoids comparing software AES-CM against OpenSSL-accelerated AES-GCM.
mkdir -p build/webrtc_bench/srtp-include
if [[ ! -e build/webrtc_bench/srtp-include/srtp2 ]]; then
  ln -s "$PEER/third_party/libsrtp/include" build/webrtc_bench/srtp-include/srtp2
fi
cmake -S github_repos/libdatachannel -B build/webrtc_bench/native \
  -DCMAKE_BUILD_TYPE=Release -DNO_EXAMPLES=ON -DNO_TESTS=ON \
  -DNO_WEBSOCKET=ON -DNO_MEDIA=OFF -DBUILD_SHARED_LIBS=OFF \
  -DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR:-/opt/homebrew/opt/openssl@3}" \
  -DUSE_SYSTEM_SRTP=ON \
  -DSRTP2_INCLUDE_DIRS="$ROOT/build/webrtc_bench/srtp-include" \
  -DSRTP_LIBRARIES="$PEER/third_party/libsrtp/build_host/libsrtp2.a"
cmake --build build/webrtc_bench/native --target datachannel-static -j "${JOBS:-6}"
cmake -S tools/webrtc_bench -B build/webrtc_bench/harness \
  -DCMAKE_BUILD_TYPE=Release -DBENCH_DIAGNOSTICS=OFF
cmake --build build/webrtc_bench/harness -j "${JOBS:-6}"
