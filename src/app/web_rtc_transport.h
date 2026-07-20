#pragma once

#include "../webrtc/peer_manager.h"
#include "ice_candidate_processor.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace lunar::app {

using CancelCallback = std::function<bool()>;

class WebRtcTransport {
public:
    explicit WebRtcTransport(std::unique_ptr<webrtc::PeerManager> peer);
    ~WebRtcTransport();

    bool initialize();
    void setCallbacks(const webrtc::PeerCallbacks& callbacks);
    std::string createOffer();
    void setRemoteAnswer(const std::string& answer);
    void setRemoteAnswer(const std::string& answer,
                         const std::vector<IceCandidatePayload>& candidates);
    std::vector<webrtc::IceCandidate> gatherLocalCandidates(
        std::chrono::milliseconds stable_window,
        std::chrono::milliseconds timeout,
        const CancelCallback& cancel);
    void addRemoteCandidates(const std::vector<IceCandidatePayload>& candidates);
    bool waitDataChannels(std::chrono::milliseconds timeout,
                          const CancelCallback& cancel);
    bool sendInputData(const uint8_t* data, size_t len);
    bool sendControlData(const uint8_t* data, size_t len);
    bool sendMessageData(const uint8_t* data, size_t len);
    bool requestVideoKeyframe();
    bool isDataChannelReady() const;
    void setMediaEnabled(bool enabled);
    PeerConnectionMediaStats getMediaStats() const;
    bool isConnected() const;
    void processEvents();
    void disconnect();

private:
    std::unique_ptr<webrtc::PeerManager> peer_;
    IceCandidateProcessor ice_processor_;
};

} // namespace lunar::app
