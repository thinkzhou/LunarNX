cmake_minimum_required(VERSION 3.21)
project(lunarnx_webrtc_bench LANGUAGES C CXX)
option(BENCH_DIAGNOSTICS "Enable bounded libpeer handshake diagnostics" OFF)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LDC_BUILD "${ROOT}/build/webrtc_bench/native" CACHE PATH "Native libdatachannel build")
set(PEER "${ROOT}/lib/libpeer")
add_executable(datachannel_bench transport_bench.cpp)
target_include_directories(datachannel_bench PRIVATE "${ROOT}/github_repos/libdatachannel/include")
target_compile_definitions(datachannel_bench PRIVATE RTC_ENABLE_WEBSOCKET=0 RTC_ENABLE_MEDIA=1 RTC_STATIC)
set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3 CACHE PATH "Native OpenSSL root")
find_package(OpenSSL REQUIRED)
target_link_libraries(datachannel_bench PRIVATE
  "${LDC_BUILD}/libdatachannel-static.a"
  "${LDC_BUILD}/deps/libjuice/libjuice-static.a"
  "${LDC_BUILD}/deps/usrsctp/usrsctplib/libusrsctp.a"
  "${PEER}/third_party/libsrtp/build_host/libsrtp2.a" OpenSSL::SSL OpenSSL::Crypto)
set(PEER_SOURCES address agent base64 dtls_srtp ice mdns peer_connection peer ports rtcp rtp sctp sdp socket stun ssl_transport utils)
add_executable(libpeer_bench transport_bench.cpp)
foreach(source IN LISTS PEER_SOURCES)
  target_sources(libpeer_bench PRIVATE "${PEER}/src/${source}.c")
endforeach()
target_include_directories(libpeer_bench PRIVATE "${PEER}/src"
  "${PEER}/third_party/mbedtls/include" "${PEER}/third_party/libsrtp/include"
  "${PEER}/third_party/libsrtp/crypto/include"
  "${PEER}/third_party/coreHTTP/source/interface")
target_compile_options(libpeer_bench PRIVATE
  "SHELL:-idirafter '${PEER}/include'" "SHELL:-idirafter '${PEER}/include/sys'")
target_compile_definitions(libpeer_bench PRIVATE BENCH_LIBPEER CONFIG_USE_LWIP=0 CONFIG_IPV6=0
  CONFIG_USE_USRSCTP=0 CONFIG_DTLS_USE_ECDSA=1 CONFIG_IFACE_PREFIX="en0" LOG_LEVEL=-1
  PEER_CONNECTION_ENABLE_DEBUG_LOGS=$<BOOL:${BENCH_DIAGNOSTICS}>)
target_link_libraries(libpeer_bench PRIVATE
  "${PEER}/third_party/mbedtls/build_host/library/libmbedtls.a"
  "${PEER}/third_party/mbedtls/build_host/library/libmbedx509.a"
  "${PEER}/third_party/mbedtls/build_host/library/libmbedcrypto.a"
  "${PEER}/third_party/libsrtp/build_host/libsrtp2.a")
