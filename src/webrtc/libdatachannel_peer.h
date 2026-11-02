#pragma once
#include <peer_connection.h>
#include <memory>
#include <chrono>
#include <string>

namespace lunar::webrtc {
// Network-only adapter. All callbacks are delivered by loop() on the same
// owner thread as the legacy peer, including media/jitter callbacks.
class LibdatachannelPeer {
public:
    LibdatachannelPeer();
    ~LibdatachannelPeer();
    bool initialize(const PeerConfiguration& config,
                    void (*candidate)(char*, void*),
                    void (*state)(PeerConnectionState, void*),
                    void (*message)(char*, size_t, void*, uint16_t));
    std::string createOffer();
    void setRemoteAnswer(const std::string& sdp);
    void addIceCandidate(const std::string& candidate);
    bool channelsReady() const;
    int send(uint16_t sid, const uint8_t* data, size_t size, bool binary);
    int sendNack(uint16_t pid, uint16_t blp);
    int requestKeyframe();
    int sendFeedback(uint8_t fraction, uint32_t lost, uint32_t highest, uint32_t bitrate);
    void setMediaEnabled(bool enabled);
    int loop();
    // Owner-thread only, between loop() calls. Pending gated media is ignored;
    // cancellation/timer service is bounded by the caller's deadline.
    void waitForEventsUntil(std::chrono::steady_clock::time_point deadline);
    PeerConnectionMediaStats stats() const;
    void close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
