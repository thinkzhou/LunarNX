cmake_minimum_required(VERSION 3.20)
project(LunarNX VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Platform detection
# =============================================================================
if(DEFINED ENV{DEVKITPRO} AND NOT DEFINED PLATFORM_DESKTOP)
    set(PLATFORM_SWITCH ON)
    message(STATUS "Platform: Switch (auto-detected)")
else()
    set(PLATFORM_DESKTOP ON)
    set(PLATFORM_SWITCH OFF)
    message(STATUS "Platform: Desktop")
endif()

# =============================================================================
# Source files (shared between Switch and Desktop)
# =============================================================================
set(LUNARNX_SOURCES
    lib/qrcodegen/qrcodegen.cpp

    # Auth
    src/auth/auth_manager.cpp
    src/auth/token_store.cpp
    src/auth/xbox_signing.cpp

    # API
    src/api/xbox_api_client.cpp
    src/api/http_client.cpp

    # WebRTC
    src/webrtc/peer_manager.cpp

    # Stream
    src/stream/stream_backend_provider.cpp
    src/stream/media_pipeline.cpp
    src/stream/video_decoder.cpp
    src/stream/audio_decoder.cpp
    src/stream/video_renderer.cpp
    src/stream/software_video_frame.cpp
    src/stream/audio_player.cpp
    src/stream/av_sync.cpp

    # Input
    src/input/gamepad_reader.cpp
    src/input/xinput_encoder.cpp
    src/input/rumble_controller.cpp

    # App
    src/app/ice_candidate_processor.cpp
    src/app/stream_controller.cpp
    src/app/web_rtc_transport.cpp
    src/app/xbox_channel_manager.cpp
    src/app/xbox_session_client.cpp
    src/app/xbox_stream_session.cpp

    # PlayStation
    src/ps/chiaki_log_adapter.cpp
    src/ps/ps_credentials.cpp
    src/ps/ps_discovery.cpp
    src/ps/ps_registration.cpp
    src/ps/ps_manager.cpp
    src/ps/ps_input_mapper.cpp
    src/ps/ps_media_bridge.cpp
    src/ps/ps_stream_session.cpp
    src/ps/ps_stream_controller.cpp
    src/ps/psn_auth_manager.cpp
    src/ps/psn_auth_utils.cpp
    src/ps/ps_console_repository.cpp
    src/ps/ps_connection_plan.cpp
    src/ps/ps_remote_connector.cpp

    # JSON
    lib/cJSON.c

    # libpeer
    lib/libpeer/src/address.c
    lib/libpeer/src/agent.c
    lib/libpeer/src/base64.c
    lib/libpeer/src/dtls_srtp.c
    lib/libpeer/src/ice.c
    lib/libpeer/src/mdns.c
    lib/libpeer/src/peer_connection.c
    lib/libpeer/src/peer.c
    lib/libpeer/src/ports.c
    lib/libpeer/src/rtcp.c
    lib/libpeer/src/rtp.c
    lib/libpeer/src/sctp.c
    lib/libpeer/src/sdp.c
    lib/libpeer/src/socket.c
    lib/libpeer/src/stun.c
    lib/libpeer/src/ssl_transport.c
    lib/libpeer/src/utils.c
)

if(PLATFORM_SWITCH)
    # =========================================================================
    # Switch target
    # =========================================================================
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/SwitchToolchain.cmake")

    list(APPEND LUNARNX_SOURCES
        src/main.cpp
        src/ui/auth_activity.cpp
        src/ui/main_activity.cpp
        src/ui/cloud_library_model.cpp
        src/ui/cloud_game_card.cpp
        src/ui/qr_code_view.cpp
        src/ui/platform_activity.cpp
        src/ui/ps_activity.cpp
        src/ui/ps_settings_activity.cpp
        src/ui/ps_registration_activity.cpp
        src/ui/psn_login_activity.cpp
        src/ps/psn_callback_server.cpp
        src/auth/xbox_login_server.cpp
        src/ui/stream_view.cpp
        src/ui/perf_overlay.cpp
        lib/stubs/mbedtls_timing.c
        lib/stubs/dav1d_stubs.c
    )

    add_executable(LunarNX ${LUNARNX_SOURCES})

    target_compile_definitions(LunarNX PRIVATE
        CHIAKI_LIB_ENABLE_MBEDTLS
        CHIAKI_LIB_ENABLE_OPUS=1
    )

    target_include_directories(LunarNX PRIVATE
        src/
        lib/
        lib/switch/include/
        "${LIBNX}/include"
        "${PORTLIBS}/include"
        "${PORTLIBS}/include/curl"
        "${PORTLIBS}/include/zlib"
        "${BOREALIS_DIR}/library/include"
        "${BOREALIS_DIR}/library/include/borealis/extern"
        "${BOREALIS_DIR}/library/lib/extern/fmt/include"
        "${BOREALIS_DIR}/library/lib/extern/yoga"
        "${LIBPEER_DIR}/include"
        "${LIBPEER_DIR}/include/sys"
        "${LIBPEER_DIR}/src"
        "${LIBPEER_DIR}/third_party/mbedtls/include"
        "${LIBPEER_DIR}/third_party/libsrtp/include"
        "${LIBPEER_DIR}/third_party/coreHTTP/source/interface"
    )

    target_link_libraries(LunarNX PRIVATE
        ${LIBNX} ${DEKO3D} ${CURL} ${MBEDTLS} ${ZLIB} ${NX}
        avcodec avutil swresample swscale opus
        borealis fmt yogacore tinyxml2 deko3d srtp2 mbedtls mbedcrypto mbedx509
    )

else()
    # =========================================================================
    # Desktop target (macOS/Linux testing)
    # =========================================================================
    list(APPEND LUNARNX_SOURCES
        src/platform/desktop_main.cpp
    )

    # System dependencies
    find_package(OpenSSL REQUIRED)
    find_package(CURL REQUIRED)

    if(APPLE)
        find_path(SDL2_INCLUDE_DIR
            NAMES SDL.h
            PATHS /opt/homebrew/include/SDL2 /usr/local/include/SDL2
            NO_DEFAULT_PATH
            REQUIRED
        )
        find_library(SDL2_LIBRARY
            NAMES SDL2
            PATHS /opt/homebrew/lib /usr/local/lib
            NO_DEFAULT_PATH
            REQUIRED
        )
        set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
        set(SDL2_LIBRARIES ${SDL2_LIBRARY})
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(SDL2_PC QUIET sdl2)
        endif()

        if(SDL2_PC_FOUND)
            set(SDL2_INCLUDE_DIRS ${SDL2_PC_INCLUDE_DIRS})
            set(SDL2_LIBRARIES ${SDL2_PC_LINK_LIBRARIES})
        else()
            find_path(SDL2_INCLUDE_DIR NAMES SDL.h PATH_SUFFIXES SDL2 REQUIRED)
            find_library(SDL2_LIBRARY NAMES SDL2 REQUIRED)
            set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
            set(SDL2_LIBRARIES ${SDL2_LIBRARY})
        endif()
    endif()

    find_library(AVCODEC_LIB avcodec REQUIRED)
    find_library(AVUTIL_LIB avutil REQUIRED)
    find_library(SWRESAMPLE_LIB swresample REQUIRED)
    find_library(OPUS_LIB opus REQUIRED)
    find_path(AVCODEC_INCLUDE libavcodec/avcodec.h REQUIRED)
    find_path(AVUTIL_INCLUDE libavutil/frame.h REQUIRED)
    find_path(OPUS_INCLUDE opus/opus_multistream.h REQUIRED)

    add_executable(LunarNX_Desktop ${LUNARNX_SOURCES})

    target_compile_definitions(LunarNX_Desktop PRIVATE
        PLATFORM_DESKTOP
        CONFIG_USE_LWIP=0
        CONFIG_USE_USRSCTP=0
        CONFIG_DTLS_USE_ECDSA=1
    )

    target_include_directories(LunarNX_Desktop PRIVATE
        src/
        lib/
        lib/libpeer/include
        lib/libpeer/include/sys
        lib/libpeer/src
        lib/libpeer/build/dist/include
        lib/libpeer/build/dist/include/srtp2
        lib/libpeer/third_party/coreHTTP/source/interface
        ${SDL2_INCLUDE_DIRS}
        ${AVCODEC_INCLUDE}
        ${AVUTIL_INCLUDE}
        ${OPUS_INCLUDE}
    )
    if(APPLE)
        target_compile_options(LunarNX_Desktop PRIVATE
            -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1
        )
    endif()

    target_link_directories(LunarNX_Desktop PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/lib/libpeer/build/dist/lib
    )

    target_link_libraries(LunarNX_Desktop PRIVATE
        OpenSSL::SSL OpenSSL::Crypto CURL::libcurl
        ${SDL2_LIBRARIES}
        ${AVCODEC_LIB} ${AVUTIL_LIB} ${SWRESAMPLE_LIB} ${OPUS_LIB}
        mbedtls mbedcrypto mbedx509 srtp2
    )

    message(STATUS "Desktop target: LunarNX_Desktop")

    # =========================================================================
    # rho — Local video player for decode pipeline testing
    # =========================================================================
    find_library(AVFORMAT_LIB avformat REQUIRED)
    find_path(AVFORMAT_INCLUDE libavformat/avformat.h REQUIRED)

    set(RHO_SOURCES
        src/rho/rho_main.cpp
        src/stream/video_decoder.cpp
        src/stream/video_renderer.cpp
    )

    add_executable(Rho_Desktop ${RHO_SOURCES})

    target_compile_definitions(Rho_Desktop PRIVATE PLATFORM_DESKTOP)

    target_include_directories(Rho_Desktop PRIVATE
        src/
        ${SDL2_INCLUDE_DIRS}
        ${AVCODEC_INCLUDE}
        ${AVUTIL_INCLUDE}
        ${AVFORMAT_INCLUDE}
    )
    if(APPLE)
        target_compile_options(Rho_Desktop PRIVATE
            -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1
        )
    endif()

    target_link_libraries(Rho_Desktop PRIVATE
        ${SDL2_LIBRARIES}
        ${AVCODEC_LIB} ${AVUTIL_LIB} ${AVFORMAT_LIB}
    )

    message(STATUS "Desktop target: Rho_Desktop")
endif()
