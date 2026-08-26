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
    // Match Green-NX and the official legacy XStreaming setup. Protocol 1.0
    // carries a contiguous sequence, so input must remain reliable/ordered;
    // ordinary loss on an unreliable channel leaves the server waiting for
    // the missing sequence and presents as permanently lost controls.
    {"control", "controlV1", 0, true, -1},
    {"input", "1.0", 2, true, -1},
    {"message", "messageV1", 4, true, -1},
    {"chat", "chatV1", 6, true, -1},
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
