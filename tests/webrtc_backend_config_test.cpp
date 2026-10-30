#include "webrtc/webrtc_backend.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#ifndef LUNARNX_ENABLE_LIBDATACHANNEL
#define LUNARNX_ENABLE_LIBDATACHANNEL 0
#endif
using lunar::webrtc::WebRtcBackend;
using lunar::webrtc::loadWebRtcBackend;
int main(int argc, char** argv) {
    assert(argc == 2);
    const auto original = std::filesystem::current_path();
    const auto test = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(test);
    std::filesystem::current_path(test);
    std::filesystem::remove("config.json");
    assert(loadWebRtcBackend() == WebRtcBackend::Libpeer);
    const auto check = [](const std::string& json, WebRtcBackend expected) {
        { std::ofstream file("config.json"); file << json; }
        assert(loadWebRtcBackend() == expected);
    };
    check("{}", WebRtcBackend::Libpeer);
    constexpr auto experimental = LUNARNX_ENABLE_LIBDATACHANNEL
        ? WebRtcBackend::Libdatachannel : WebRtcBackend::Libpeer;
    check("{\"xbox_webrtc_backend\":\"libdatachannel\",\"resolution\":1080}", experimental);
    check("{\"xbox_webrtc_backend\":\"libpeer\"}", WebRtcBackend::Libpeer);
    check("{\"xbox_webrtc_backend\":\"unknown\"}", WebRtcBackend::Libpeer);
    check("{\"xbox_webrtc_backend\":true}", WebRtcBackend::Libpeer);
    check("{\"xbox_webrtc_backend\":", WebRtcBackend::Libpeer);
    check(std::string(70000, 'x'), WebRtcBackend::Libpeer);
    std::filesystem::remove("config.json");
    std::filesystem::current_path(original);
}
