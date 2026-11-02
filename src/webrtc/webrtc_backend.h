#pragma once
#include <cstring>

namespace lunar::webrtc {
enum class WebRtcBackend { Libpeer, Libdatachannel };
inline WebRtcBackend parseWebRtcBackend(const char* value) {
    return value && std::strcmp(value, "libdatachannel") == 0
        ? WebRtcBackend::Libdatachannel : WebRtcBackend::Libpeer;
}
inline const char* webRtcBackendName(WebRtcBackend backend) {
    return backend == WebRtcBackend::Libdatachannel ? "libdatachannel" : "libpeer";
}
// Read only at connection creation; malformed/missing settings retain libpeer.
WebRtcBackend loadWebRtcBackend();
}
