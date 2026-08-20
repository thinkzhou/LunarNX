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
    // Gamepad state is a realtime stream: the application queue coalesces an
    // older unsent complete snapshot into the newest one. The channel itself
    // must stay reliable and ordered because XStreaming input packets carry a
    // contiguous sequence shared with the startup metadata packet.
    {"input", "1.0", 0, true, -1},
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
