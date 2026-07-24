#include "web_rtc_transport.h"
#include "../diagnostics.h"

#include <thread>

namespace lunar::app {
namespace {

constexpr std::chrono::milliseconds kDataChannelSettle{500};
constexpr std::chrono::seconds kConnectedDataChannelTimeout{12};

std::string answerWithInlineCandidates(
    std::string answer,
    const std::vector<std::string>& candidates) {
    if (candidates.empty()) {
        return answer;
    }
    if (!answer.empty() && answer.back() != '\n') {
        answer += "\r\n";
    }
    for (const auto& candidate : candidates) {
        answer += candidate;
        if (answer.size() < 2 ||
            answer.substr(answer.size() - 2) != "\r\n") {
            answer += "\r\n";
        }
    }
    answer += "a=end-of-candidates\r\n";
    return answer;
}

} // namespace

WebRtcTransport::WebRtcTransport(std::unique_ptr<webrtc::PeerManager> peer)
    : peer_(std::move(peer)) {}

WebRtcTransport::~WebRtcTransport() = default;

bool WebRtcTransport::initialize() {
    return peer_ && peer_->initialize();
}

void WebRtcTransport::setCallbacks(const webrtc::PeerCallbacks& callbacks) {
    if (peer_) {
        peer_->setCallbacks(callbacks);
    }
}

std::string WebRtcTransport::createOffer() {
    lunar::diagnosticLog("webrtc-transport", "createOffer begin peer=%s", peer_ ? "true" : "false");
    auto offer = peer_ ? peer_->createOffer() : "";
    lunar::diagnosticLog("webrtc-transport", "createOffer done len=%zu", offer.size());
    return offer;
}

void WebRtcTransport::setRemoteAnswer(const std::string& answer) {
    if (peer_) {
        lunar::diagnosticLog("webrtc-transport", "setRemoteAnswer begin len=%zu", answer.size());
        peer_->setRemoteAnswer(answer);
        lunar::diagnosticLog("webrtc-transport", "setRemoteAnswer done");
    }
}

void WebRtcTransport::setRemoteAnswer(
    const std::string& answer,
    const std::vector<IceCandidatePayload>& candidates) {
    const auto lines = ice_processor_.toLibPeerLines(candidates);
    const auto enriched = answerWithInlineCandidates(answer, lines);
    lunar::diagnosticLog("webrtc-transport",
                         "setRemoteAnswer with candidates answer_len=%zu candidate_count=%zu enriched_len=%zu",
                         answer.size(), lines.size(), enriched.size());
    setRemoteAnswer(enriched);
}

std::vector<webrtc::IceCandidate> WebRtcTransport::gatherLocalCandidates(
    std::chrono::milliseconds stable_window,
    std::chrono::milliseconds timeout,
    const CancelCallback& cancel) {
    if (!peer_) {
        return {};
    }

    auto start = std::chrono::steady_clock::now();
    auto last_change = start;
    size_t last_count = 0;
    std::vector<webrtc::IceCandidate> candidates;

    while (true) {
        if (cancel && cancel()) {
            return {};
        }

        peer_->processEvents();
        candidates = peer_->getLocalCandidates();

        const auto now = std::chrono::steady_clock::now();
        if (candidates.size() != last_count) {
            last_count = candidates.size();
            last_change = now;
        }

        const auto stable_for =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_change);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start);

        if (stable_for >= stable_window && !candidates.empty()) {
            lunar::diagnosticLog("webrtc-transport", "gatherLocalCandidates stable count=%zu elapsed_ms=%lld",
                                 candidates.size(),
                                 static_cast<long long>(elapsed.count()));
            break;
        }
        if (elapsed >= timeout) {
            lunar::diagnosticLog("webrtc-transport", "gatherLocalCandidates timeout count=%zu elapsed_ms=%lld",
                                 candidates.size(),
                                 static_cast<long long>(elapsed.count()));
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return candidates;
}

void WebRtcTransport::addRemoteCandidates(
    const std::vector<IceCandidatePayload>& candidates) {
    if (!peer_) {
        return;
    }
    for (const auto& candidate : ice_processor_.toLibPeerLines(candidates)) {
        lunar::diagnosticLog("webrtc-transport", "addRemoteCandidate len=%zu", candidate.size());
        peer_->addIceCandidate(candidate);
    }
    lunar::diagnosticLog("webrtc-transport", "addRemoteCandidates done count=%zu", candidates.size());
}

bool WebRtcTransport::waitDataChannels(std::chrono::milliseconds timeout,
                                       const CancelCallback& cancel) {
    if (!peer_) {
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    auto connected_at = std::chrono::steady_clock::time_point{};
    while (true) {
        if (cancel && cancel()) {
            return false;
        }

        peer_->processEvents();
        const auto now = std::chrono::steady_clock::now();
        if (peer_->isDataChannelReady()) {
            const auto settle_start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - settle_start < kDataChannelSettle) {
                if (cancel && cancel()) {
                    return false;
                }
                peer_->processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            lunar::diagnosticLog("webrtc-transport", "waitDataChannels ready elapsed_ms=%lld",
                                 static_cast<long long>(
                                     std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - start).count()));
            return true;
        }
        if (peer_->isConnected()) {
            if (connected_at.time_since_epoch().count() == 0) {
                connected_at = now;
            } else if (now - connected_at >= kConnectedDataChannelTimeout) {
                lunar::diagnosticLog(
                    "webrtc-transport",
                    "waitDataChannels dead media path connected_ms=%lld",
                    static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - connected_at).count()));
                return false;
            }
        } else {
            connected_at = {};
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start);
        if (elapsed >= timeout) {
            lunar::diagnosticLog("webrtc-transport", "waitDataChannels timeout elapsed_ms=%lld",
                                 static_cast<long long>(elapsed.count()));
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

bool WebRtcTransport::sendInputData(const uint8_t* data, size_t len) {
    return peer_ && peer_->sendInputData(data, len);
}

bool WebRtcTransport::sendControlData(const uint8_t* data, size_t len) {
    lunar::diagnosticLog("webrtc-transport", "send control begin len=%zu", len);
    const bool sent = peer_ && peer_->sendControlData(data, len);
    lunar::diagnosticLog("webrtc-transport", "send control result=%s",
                         sent ? "true" : "false");
    return sent;
}

bool WebRtcTransport::sendMessageData(const uint8_t* data, size_t len) {
    lunar::diagnosticLog("webrtc-transport", "send message begin len=%zu", len);
    const bool sent = peer_ && peer_->sendMessageData(data, len);
    lunar::diagnosticLog("webrtc-transport", "send message result=%s",
                         sent ? "true" : "false");
    return sent;
}

bool WebRtcTransport::requestVideoKeyframe() {
    const bool sent = peer_ && peer_->requestVideoKeyframe();
    lunar::diagnosticLog("webrtc-transport", "send RTCP PLI result=%s",
                         sent ? "true" : "false");
    return sent;
}

bool WebRtcTransport::sendReceiverFeedback(uint32_t bitrate_bps) {
    const bool sent = peer_ && peer_->sendReceiverFeedback(bitrate_bps);
    lunar::diagnosticLog("webrtc-transport",
                         "send RTCP RR+REMB bitrate_bps=%u result=%s",
                         bitrate_bps,
                         sent ? "true" : "false");
    return sent;
}

bool WebRtcTransport::isDataChannelReady() const {
    return peer_ && peer_->isDataChannelReady();
}

void WebRtcTransport::setMediaEnabled(bool enabled) {
    if (peer_) {
        peer_->setMediaEnabled(enabled);
    }
}

PeerConnectionMediaStats WebRtcTransport::getMediaStats() const {
    return peer_ ? peer_->getMediaStats() : PeerConnectionMediaStats{};
}

bool WebRtcTransport::isConnected() const {
    return peer_ && peer_->isConnected();
}

void WebRtcTransport::processEvents() {
    if (peer_) {
        peer_->processEvents();
    }
}

void WebRtcTransport::disconnect() {
    if (peer_) {
        peer_->disconnect();
    }
}

} // namespace lunar::app
