#!/usr/bin/env bash
set -uo pipefail

source /opt/devkitpro/switchvars.sh

ROOT_DIR="/work"
SRC_DIR="${ROOT_DIR}/github_repos/libdatachannel"
OUT_DIR="${ROOT_DIR}/tools/libdatachannel_switch_spike"
LOG_DIR="${OUT_DIR}/logs"
BUILD_ROOT="${OUT_DIR}/build"
COMPAT_INCLUDE="${OUT_DIR}/compat/include"
SUMMARY="${OUT_DIR}/attempt_summary.tsv"
DEPS_ROOT="${BUILD_ROOT}/deps"
MBEDTLS3_TAG="${MBEDTLS3_TAG:-mbedtls-3.6.6}"
MBEDTLS3_SRC="${DEPS_ROOT}/mbedtls-${MBEDTLS3_TAG}"
MBEDTLS3_BUILD="${BUILD_ROOT}/mbedtls3-build"
MBEDTLS3_INSTALL="${OUT_DIR}/install/mbedtls3"

mkdir -p "${LOG_DIR}" "${BUILD_ROOT}" "${DEPS_ROOT}"

printf "attempt\tconfigure\tbuild\ttarget\tbuild_dir\n" > "${SUMMARY}"

common_cmake_args=(
  -S "${SRC_DIR}"
  -G Ninja
  -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_C_FLAGS="${CFLAGS} -I${COMPAT_INCLUDE} -include ${COMPAT_INCLUDE}/switch_spike_compat.h"
  -DCMAKE_CXX_FLAGS="${CXXFLAGS} -I${COMPAT_INCLUDE} -include ${COMPAT_INCLUDE}/switch_spike_compat.h"
  -DBUILD_SHARED_LIBS=OFF
  -DBUILD_SHARED_DEPS_LIBS=OFF
  -DPREFER_SYSTEM_LIB=OFF
  -DUSE_SYSTEM_PLOG=OFF
  -DUSE_SYSTEM_USRSCTP=OFF
  -DUSE_SYSTEM_JUICE=OFF
  -DUSE_NICE=OFF
  -DUSE_MBEDTLS=ON
  -DNO_EXAMPLES=ON
  -DNO_TESTS=ON
  -DRTC_UPDATE_VERSION_HEADER=OFF
)

prepare_mbedtls3() {
  local fetch_log="${LOG_DIR}/mbedtls3.fetch.log"
  local configure_log="${LOG_DIR}/mbedtls3.configure.log"
  local build_log="${LOG_DIR}/mbedtls3.build.log"
  local configure_status=0
  local build_status=0

  if [ ! -d "${MBEDTLS3_SRC}/.git" ]; then
    rm -rf "${MBEDTLS3_SRC}"
    git clone --depth 1 --branch "${MBEDTLS3_TAG}" \
      https://github.com/Mbed-TLS/mbedtls.git "${MBEDTLS3_SRC}" >"${fetch_log}" 2>&1
  else
    git -C "${MBEDTLS3_SRC}" rev-parse --short HEAD >"${fetch_log}" 2>&1
  fi

  git -C "${MBEDTLS3_SRC}" submodule update --init --recursive >>"${fetch_log}" 2>&1
  python3 "${MBEDTLS3_SRC}/scripts/config.py" set MBEDTLS_NO_PLATFORM_ENTROPY >>"${fetch_log}" 2>&1
  python3 "${MBEDTLS3_SRC}/scripts/config.py" unset MBEDTLS_HAVE_TIME_DATE >>"${fetch_log}" 2>&1
  python3 "${MBEDTLS3_SRC}/scripts/config.py" unset MBEDTLS_HAVE_TIME >>"${fetch_log}" 2>&1
  python3 "${MBEDTLS3_SRC}/scripts/config.py" unset MBEDTLS_TIMING_C >>"${fetch_log}" 2>&1
  python3 "${MBEDTLS3_SRC}/scripts/config.py" unset MBEDTLS_NET_C >>"${fetch_log}" 2>&1
  python3 "${MBEDTLS3_SRC}/scripts/config.py" set MBEDTLS_SSL_DTLS_SRTP >>"${fetch_log}" 2>&1

  rm -rf "${MBEDTLS3_BUILD}" "${MBEDTLS3_INSTALL}"

  cmake -S "${MBEDTLS3_SRC}" -B "${MBEDTLS3_BUILD}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${MBEDTLS3_INSTALL}" \
    -DENABLE_TESTING=OFF \
    -DENABLE_PROGRAMS=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON >"${configure_log}" 2>&1
  configure_status=$?

  if [ "${configure_status}" -eq 0 ]; then
    cmake --build "${MBEDTLS3_BUILD}" --target install -j"$(nproc)" >"${build_log}" 2>&1
    build_status=$?
  else
    build_status=NA
    printf "configure failed; build not attempted\n" > "${build_log}"
  fi

  printf "%s\t%s\t%s\t%s\t%s\n" \
    "dependency_mbedtls3" "${configure_status}" "${build_status}" "install" "${MBEDTLS3_BUILD}" >> "${SUMMARY}"

  [ "${configure_status}" -eq 0 ] && [ "${build_status}" -eq 0 ]
}

run_attempt() {
  local name="$1"
  local target="$2"
  shift 2

  local build_dir="${BUILD_ROOT}/${name}"
  local configure_log="${LOG_DIR}/${name}.configure.log"
  local build_log="${LOG_DIR}/${name}.build.log"
  local configure_status=0
  local build_status=0

  rm -rf "${build_dir}"

  cmake "${common_cmake_args[@]}" -B "${build_dir}" "$@" >"${configure_log}" 2>&1
  configure_status=$?

  if [ "${configure_status}" -eq 0 ]; then
    cmake --build "${build_dir}" --target "${target}" -j"$(nproc)" >"${build_log}" 2>&1
    build_status=$?
  else
    build_status=NA
    printf "configure failed; build not attempted\n" > "${build_log}"
  fi

  printf "%s\t%s\t%s\t%s\t%s\n" \
    "${name}" "${configure_status}" "${build_status}" "${target}" "${build_dir}" >> "${SUMMARY}"
}

if prepare_mbedtls3; then
  common_cmake_args+=(
    -DMbedTLS_INCLUDE_DIR="${MBEDTLS3_INSTALL}/include"
    -DMbedTLS_LIBRARY="${MBEDTLS3_INSTALL}/lib/libmbedtls.a"
    -DMbedCrypto_LIBRARY="${MBEDTLS3_INSTALL}/lib/libmbedcrypto.a"
    -DMbedX509_LIBRARY="${MBEDTLS3_INSTALL}/lib/libmbedx509.a"
    -DMBEDTLS_INCLUDE_DIRS="${MBEDTLS3_INSTALL}/include"
    -DMBEDTLS_LIBRARY="${MBEDTLS3_INSTALL}/lib/libmbedtls.a"
    -DMBEDCRYPTO_LIBRARY="${MBEDTLS3_INSTALL}/lib/libmbedcrypto.a"
    -DMBEDX509_LIBRARY="${MBEDTLS3_INSTALL}/lib/libmbedx509.a"
  )
fi

run_attempt no_media_no_websocket_mbedtls3 datachannel-static \
  -DNO_MEDIA=ON \
  -DNO_WEBSOCKET=ON

run_attempt media_no_websocket_mbedtls3 datachannel-static \
  -DNO_MEDIA=OFF \
  -DNO_WEBSOCKET=ON

run_attempt media_websocket_mbedtls3 datachannel-static \
  -DNO_MEDIA=OFF \
  -DNO_WEBSOCKET=OFF

run_attempt no_media_no_websocket_openssl_portlibs datachannel-static \
  -DUSE_MBEDTLS=OFF \
  -DNO_MEDIA=ON \
  -DNO_WEBSOCKET=ON

cat "${SUMMARY}"
