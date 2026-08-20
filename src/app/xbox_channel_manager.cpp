#include "xbox_channel_manager.h"
#include "../diagnostics.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace lunar::app {

namespace {

constexpr std::chrono::milliseconds kHandshakeTimeout{1500};
constexpr std::chrono::milliseconds kReliableFlushTimeout{1500};
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

std::string jsonEscape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') escaped.push_back('\\');
        escaped.push_back(ch);
    }
    return escaped;
}

std::string wrapMessage(std::string_view target,
                        std::string_view content,
                        int counter) {
    return std::string(R"({"type":"Message","content":")") +
        jsonEscape(content) +
        R"(","id":"5c5f2b40-0000-4000-8000-00000000)" +
        std::to_string(1000 + counter) +
        R"(","target":")" + jsonEscape(target) + R"(","cv":""})";
}

std::vector<std::string> startupMessages(const StreamProfile& profile) {
    const int width = profile.width;
    const int height = profile.height;
    const int bitrate = streamProfileBitrateKbps(profile);
    const int fps = profile.fps > 0 ? profile.fps : 60;
    int counter = 0;
    std::vector<std::string> messages;
    messages.reserve(6);
    messages.push_back(wrapMessage(
        "/streaming/systemUi/configuration",
        R"({"version":[0,2,0],"systemUis":[]})", counter++));
    messages.push_back(wrapMessage(
        "/streaming/properties/clientappinstallidchanged",
        R"({"clientAppInstallId":"c97d7ee0-73b2-4239-bf1d-9d805a338429"})",
        counter++));
    messages.push_back(wrapMessage(
        "/streaming/characteristics/orientationchanged",
        R"({"orientation":0})", counter++));
    messages.push_back(wrapMessage(
        "/streaming/characteristics/touchinputenabledchanged",
        R"({"touchInputEnabled":false})", counter++));
    messages.push_back(wrapMessage(
        "/streaming/characteristics/clientdevicecapabilities",
        std::string(R"({"supportsCustomResolution":true,"supportsHevc":false,"supportsHdr":false,"supportsFps":)") +
            std::to_string(fps) +
            R"(,"maxWidth":)" + std::to_string(width) +
            R"(,"maxHeight":)" + std::to_string(height) +
            R"(,"maxBitrateKbps":)" + std::to_string(bitrate) +
            R"(,"video":{"width":)" + std::to_string(width) +
            R"(,"height":)" + std::to_string(height) +
            R"(,"maxWidth":)" + std::to_string(width) +
            R"(,"maxHeight":)" + std::to_string(height) +
            R"(,"maxBitrateKbps":)" + std::to_string(bitrate) + "}}",
        counter++));
    messages.push_back(wrapMessage(
        "/streaming/characteristics/dimensionschanged",
        std::string(R"({"horizontal":)") + std::to_string(width) +
            R"(,"vertical":)" + std::to_string(height) +
            R"(,"preferredWidth":)" + std::to_string(width) +
            R"(,"preferredHeight":)" + std::to_string(height) +
            R"(,"safeAreaLeft":0,"safeAreaTop":0,"safeAreaRight":)" +
            std::to_string(width) + R"(,"safeAreaBottom":)" +
            std::to_string(height) + R"(,"supportsCustomResolution":true})",
        counter++));
    return messages;
}

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

bool XboxChannelManager::startProtocol(const StreamProfile& profile,
                                       const uint8_t* metadata,
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
    // The sends above are queued so libpeer remains single-owner and no
    // callback performs a synchronous DTLS write. Flush them before preserving
    // XStreaming's required gamepad-add delay.
    if (!flushReliableData(cancel)) {
        lunar::diagnosticLog("xbox-channel", "authorization flush failed");
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

    const auto startup = startupMessages(profile);
    for (const auto& message : startup) {
        if (!transport_.sendMessageData(
                reinterpret_cast<const uint8_t*>(message.data()), message.size())) {
            lunar::diagnosticLog("xbox-channel", "startup capability send failed");
            return false;
        }
    }
    lunar::diagnosticLog("xbox-channel",
                         "startup capabilities sent width=%d height=%d fps=%d bitrate_kbps=%d messages=%zu",
                         profile.width,
                         profile.height,
                         profile.fps,
                         streamProfileBitrateKbps(profile),
                         startup.size());

    if (metadata && metadata_len > 0) {
        lunar::diagnosticLog("xbox-channel", "metadata send begin len=%zu", metadata_len);
        const bool sent = transport_.sendInputData(metadata, metadata_len);
        lunar::diagnosticLog("xbox-channel", "metadata send result=%s",
                             sent ? "true" : "false");
        std::fprintf(stderr, "[ctrl] Metadata send result=%s\n", sent ? "true" : "false");
        std::fflush(stderr);
        if (!sent) return false;
    }
    const bool flushed = flushReliableData(cancel);
    lunar::diagnosticLog("xbox-channel", "startup reliable flush result=%s",
                         flushed ? "true" : "false");
    return flushed;
}

bool XboxChannelManager::sendInputPacket(const uint8_t* data,
                                         size_t len,
                                         bool reliable) {
    return reliable ? transport_.sendInputData(data, len)
                    : transport_.sendLatestInputData(data, len);
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
    const bool pli_sent = transport_.requestVideoKeyframe();
    // Queue PLI first so data-channel backpressure cannot delay the independent
    // SRTCP recovery signal behind the advisory Xbox control message.
    const bool control_sent = sendControlMessage(message);
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
        if (transport_.consumeDataChannelFailure()) {
            lunar::dropDiagnosticLog("xbox-channel",
                                     "message_handshake_send_failed=1");
            return false;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed >= kHandshakeTimeout) {
            return true;
        }

        std::this_thread::sleep_for(kPollInterval);
    }
    return true;
}

bool XboxChannelManager::flushReliableData(const CancelCallback& cancel) {
    const auto deadline = std::chrono::steady_clock::now() +
                          kReliableFlushTimeout;
    while (transport_.hasPendingReliableData()) {
        if (cancel && cancel()) return false;
        transport_.processEvents();
        if (transport_.consumeDataChannelFailure()) {
            lunar::dropDiagnosticLog("xbox-channel",
                                     "reliable_data_send_failed=1");
            return false;
        }
        if (!transport_.hasPendingReliableData()) return true;
        if (std::chrono::steady_clock::now() >= deadline) {
            lunar::dropDiagnosticLog("xbox-channel",
                                     "reliable_data_flush_timeout_ms=%lld",
                                     static_cast<long long>(
                                         kReliableFlushTimeout.count()));
            return false;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return !transport_.consumeDataChannelFailure();
}

} // namespace lunar::app
