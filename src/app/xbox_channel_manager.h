#pragma once

#include "web_rtc_transport.h"
#include "stream_profile.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lunar::app {

class XboxChannelManager {
public:
    struct InputPacketSubmission {
        bool accepted = false;
        uint64_t ticket = 0;
    };

    explicit XboxChannelManager(WebRtcTransport& transport);

    void reset();
    void handleMessageChannelData(const uint8_t* data, size_t len);
    bool startProtocol(const StreamProfile& profile,
                       const uint8_t* metadata,
                       size_t metadata_len,
                       const CancelCallback& cancel);
    InputPacketSubmission sendInputPacket(const uint8_t* data,
                                          size_t len,
                                          bool reliable);
    bool sendControlMessage(std::string_view json);
    bool sendMessageHandshake();
    bool requestVideoKeyframe(bool ifr_requested);

private:
    bool waitForHandshake(const CancelCallback& cancel);
    bool flushReliableData(const CancelCallback& cancel);

    WebRtcTransport& transport_;
    std::atomic<bool> handshake_ready_{false};
};

} // namespace lunar::app
