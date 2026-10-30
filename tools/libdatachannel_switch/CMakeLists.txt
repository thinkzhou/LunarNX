cmake_minimum_required(VERSION 3.21)
project(lunarnx_datachannel_sdk LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SDK_SOURCE "${ROOT}/build/libdatachannel-sdk/source/libdatachannel")
set(PEER "${ROOT}/lib/libpeer")
option(LUNAR_SWITCH "Build the Switch SDK in devkitA64 Docker" OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(USE_SYSTEM_SRTP ON CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
if(LUNAR_SWITCH)
  set(MBED_BUILD build_switch)
  set(USE_SYSTEM_USRSCTP ON CACHE BOOL "" FORCE)
  set(Usrsctp_INCLUDE_DIR "${PEER}/third_party/usrsctp/usrsctplib" CACHE PATH "" FORCE)
  set(Usrsctp_LIBRARY "${PEER}/third_party/usrsctp/build_switch/usrsctplib/libusrsctp.a" CACHE FILEPATH "" FORCE)
  add_compile_definitions(HAVE_SCONN_LEN)
else()
  set(MBED_BUILD build_host)
endif()
set(MBED "${PEER}/third_party/mbedtls")
add_library(MbedTLS::MbedTLS INTERFACE IMPORTED GLOBAL)
set_target_properties(MbedTLS::MbedTLS PROPERTIES
 INTERFACE_INCLUDE_DIRECTORIES "${MBED}/include"
 INTERFACE_LINK_LIBRARIES "${MBED}/${MBED_BUILD}/library/libmbedtls.a;${MBED}/${MBED_BUILD}/library/libmbedx509.a;${MBED}/${MBED_BUILD}/library/libmbedcrypto.a")
add_library(libSRTP::srtp2 INTERFACE IMPORTED GLOBAL)
set_target_properties(libSRTP::srtp2 PROPERTIES
 INTERFACE_INCLUDE_DIRECTORIES "${PEER}/third_party/libsrtp/include"
 INTERFACE_LINK_LIBRARIES "${PEER}/third_party/libsrtp/${MBED_BUILD}/libsrtp2.a")
# libdatachannel's bundled finder expects an installed srtp2/srtp.h layout.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/srtp-include/srtp2")
configure_file("${PEER}/third_party/libsrtp/include/srtp.h" "${CMAKE_BINARY_DIR}/srtp-include/srtp2/srtp.h" COPYONLY)
set(SRTP2_INCLUDE_DIRS "${CMAKE_BINARY_DIR}/srtp-include" CACHE PATH "" FORCE)
set(SRTP_LIBRARIES "${PEER}/third_party/libsrtp/${MBED_BUILD}/libsrtp2.a" CACHE FILEPATH "" FORCE)
# Update the imported include path as well as satisfying FindlibSRTP.
set_target_properties(libSRTP::srtp2 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/srtp-include")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/compat")
add_subdirectory("${SDK_SOURCE}" dependency EXCLUDE_FROM_ALL)
# Both libraries are linked into one process. Their private C helpers use
# overlapping generic names, so namespace the libjuice implementations.
set(JUICE_PRIVATE_SYMBOLS agent_create agent_destroy agent_send
    agent_get_local_description agent_set_remote_description agent_update_candidate_pairs
    addr_to_string addr_set_port
    addr_get_port addr_equal addr_is_ipv4 addr_is_ipv6)
foreach(symbol IN LISTS JUICE_PRIVATE_SYMBOLS)
 target_compile_definitions(juice-static PRIVATE "${symbol}=lunar_juice_${symbol}")
endforeach()
add_library(lunar_rtc_background STATIC compat/background.cpp)
if(LUNAR_SWITCH)
 foreach(target datachannel-static datachannel juice juice-static)
  target_compile_options(${target} PRIVATE -include arpa/inet.h)
 endforeach()
endif()
