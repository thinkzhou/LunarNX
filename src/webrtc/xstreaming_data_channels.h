#pragma once

#include <cstdint>
#include <string_view>

namespace lunar::webrtc {

struct XStreamingDataChannel {
    const char* label;
    const char* protocol;
    uint16_t sid;
    bool ordered;
    int max_retransmits;  // -1 = reliable
};

inline constexpr XStreamingDataChannel kXStreamingDataChannels[] = {
    // Gamepad state is a realtime stream: a newer complete snapshot supersedes
    // an older one, so it must not wait for retransmission or ordering.
    {"input", "1.0", 0, false, 0},
    {"chat", "chatV1", 2, true, -1},
    {"control", "controlV1", 4, true, -1},
    {"message", "messageV1", 6, true, -1},
};

inline constexpr const char* xstreamingDataChannelProtocol(std::string_view label) {
    for (const auto& channel : kXStreamingDataChannels) {
        if (label == channel.label) {
            return channel.protocol;
        }
    }
    return "";
}

inline constexpr uint16_t xstreamingDataChannelSid(std::string_view label) {
    for (const auto& channel : kXStreamingDataChannels) {
        if (label == channel.label) {
            return channel.sid;
        }
    }
    return UINT16_MAX;
}

} // namespace lunar::webrtc
