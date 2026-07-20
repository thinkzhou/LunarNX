#include "xbox_channel_manager.h"
#include "../diagnostics.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace lunar::app {

namespace {

constexpr std::chrono::milliseconds kHandshakeTimeout{1500};
constexpr std::chrono::milliseconds kGamepadAddDelay{500};
constexpr std::chrono::milliseconds kPollInterval{16};

constexpr std::string_view kMessageHandshake =
    R"({"type":"Handshake","version":"messageV1","id":"lunarnx-001","cv":"0"})";
constexpr std::string_view kAuthorizationRequest =
    R"({"message":"authorizationRequest","accessKey":"4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E"})";
constexpr std::string_view kGamepadRemoved =
    R"({"message":"gamepadChanged","gamepadIndex":0,"wasAdded":false})";
constexpr std::string_view kGamepadAdded =
    R"({"message":"gamepadChanged","gamepadIndex":0,"wasAdded":true})";

bool sleepUnlessCancelled(std::chrono::milliseconds duration,
                          const CancelCallback& cancel) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < duration) {
        if (cancel && cancel()) {
            return false;
        }
        const auto remaining = duration -
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
        std::this_thread::sleep_for(remaining < kPollInterval ? remaining : kPollInterval);
    }
    return !(cancel && cancel());
}

} // namespace

XboxChannelManager::XboxChannelManager(WebRtcTransport& transport)
    : transport_(transport) {}

void XboxChannelManager::reset() {
    handshake_ready_ = false;
}

void XboxChannelManager::handleMessageChannelData(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return;
    }

    lunar::diagnosticLog("xbox-channel", "message data len=%zu", len);
    const std::string message(reinterpret_cast<const char*>(data), len);
    if (message.find("HandshakeAck") != std::string::npos) {
        handshake_ready_ = true;
        std::fprintf(stderr, "[ctrl] Message channel HandshakeAck received\n");
        lunar::diagnosticLog("xbox-channel", "message HandshakeAck received");
    }
}

bool XboxChannelManager::startProtocol(const uint8_t* metadata,
                                       size_t metadata_len,
                                       const CancelCallback& cancel) {
    if (cancel && cancel()) {
        return false;
    }

    lunar::diagnosticLog("xbox-channel", "startProtocol begin metadata_len=%zu",
                         metadata_len);
    if (!sendMessageHandshake()) {
        lunar::diagnosticLog("xbox-channel", "sendMessageHandshake failed");
        return false;
    }
    if (!waitForHandshake(cancel)) {
        lunar::diagnosticLog("xbox-channel", "waitForHandshake failed");
        return false;
    }

    if (!sendControlMessage(kAuthorizationRequest) ||
        !sendControlMessage(kGamepadRemoved)) {
        lunar::diagnosticLog("xbox-channel", "control authorization/remove failed");
        return false;
    }
    std::fprintf(stderr, "[ctrl] Control channel authorization sent\n");
    lunar::diagnosticLog("xbox-channel", "control authorization sent");
    std::fflush(stderr);

    lunar::diagnosticLog("xbox-channel", "gamepad add delay begin");
    if (!sleepUnlessCancelled(kGamepadAddDelay, cancel)) {
        lunar::diagnosticLog("xbox-channel", "gamepad add delay cancelled");
        return false;
    }
    lunar::diagnosticLog("xbox-channel", "gamepad add delay done");

    if (!sendControlMessage(kGamepadAdded)) {
        lunar::diagnosticLog("xbox-channel", "gamepad added send failed");
        return false;
    }
    std::fprintf(stderr, "[ctrl] Gamepad added sent\n");
    lunar::diagnosticLog("xbox-channel", "gamepad added sent");
    std::fflush(stderr);

    if (metadata && metadata_len > 0) {
        lunar::diagnosticLog("xbox-channel", "metadata send begin len=%zu", metadata_len);
        const bool sent = transport_.sendInputData(metadata, metadata_len);
        lunar::diagnosticLog("xbox-channel", "metadata send result=%s",
                             sent ? "true" : "false");
        std::fprintf(stderr, "[ctrl] Metadata send result=%s\n", sent ? "true" : "false");
        std::fflush(stderr);
        return sent;
    }
    lunar::diagnosticLog("xbox-channel", "startProtocol done without metadata");
    return true;
}

bool XboxChannelManager::sendInputPacket(const uint8_t* data, size_t len) {
    return transport_.sendInputData(data, len);
}

bool XboxChannelManager::sendControlMessage(std::string_view json) {
    lunar::diagnosticLog("xbox-channel", "send control len=%zu", json.size());
    const bool sent = transport_.sendControlData(
        reinterpret_cast<const uint8_t*>(json.data()),
        json.size());
    lunar::diagnosticLog("xbox-channel", "send control result=%s",
                         sent ? "true" : "false");
    return sent;
}

bool XboxChannelManager::sendMessageHandshake() {
    lunar::diagnosticLog("xbox-channel", "send message handshake begin len=%zu",
                         kMessageHandshake.size());
    const bool sent = transport_.sendMessageData(
        reinterpret_cast<const uint8_t*>(kMessageHandshake.data()),
        kMessageHandshake.size());
    if (sent) {
        std::fprintf(stderr, "[ctrl] Message channel handshake sent\n");
    }
    lunar::diagnosticLog("xbox-channel", "send message handshake result=%s",
                         sent ? "true" : "false");
    return sent;
}

bool XboxChannelManager::requestVideoKeyframe(bool ifr_requested) {
    const std::string message =
        std::string(R"({"message":"videoKeyframeRequested","ifrRequested":)") +
        (ifr_requested ? "true" : "false") + "}";
    const bool control_sent = sendControlMessage(message);
    const bool pli_sent = transport_.requestVideoKeyframe();
    lunar::diagnosticLog("xbox-channel", "keyframe request control=%s pli=%s",
                         control_sent ? "true" : "false",
                         pli_sent ? "true" : "false");
    return control_sent || pli_sent;
}

bool XboxChannelManager::waitForHandshake(const CancelCallback& cancel) {
    const auto start = std::chrono::steady_clock::now();
    while (!handshake_ready_) {
        if (cancel && cancel()) {
            return false;
        }

        transport_.processEvents();

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed >= kHandshakeTimeout) {
            return true;
        }

        std::this_thread::sleep_for(kPollInterval);
    }
    return true;
}

} // namespace lunar::app
