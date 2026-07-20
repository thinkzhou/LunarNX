#pragma once

#include "web_rtc_transport.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lunar::app {

class XboxChannelManager {
public:
    explicit XboxChannelManager(WebRtcTransport& transport);

    void reset();
    void handleMessageChannelData(const uint8_t* data, size_t len);
    bool startProtocol(const uint8_t* metadata,
                       size_t metadata_len,
                       const CancelCallback& cancel);
    bool sendInputPacket(const uint8_t* data, size_t len);
    bool sendControlMessage(std::string_view json);
    bool sendMessageHandshake();
    bool requestVideoKeyframe(bool ifr_requested);

private:
    bool waitForHandshake(const CancelCallback& cancel);

    WebRtcTransport& transport_;
    std::atomic<bool> handshake_ready_{false};
};

} // namespace lunar::app
