#pragma once

#include <cstdint>
#include <string_view>

namespace lunar::webrtc {

struct XStreamingDataChannel {
    const char* label;
    const char* protocol;
    uint16_t sid;
};

inline constexpr XStreamingDataChannel kXStreamingDataChannels[] = {
    {"input", "1.0", 0},
    {"chat", "chatV1", 2},
    {"control", "controlV1", 4},
    {"message", "messageV1", 6},
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
