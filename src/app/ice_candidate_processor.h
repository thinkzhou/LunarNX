#pragma once

#include "../webrtc/peer_manager.h"
#include "stream_profile.h"

#include <string>
#include <vector>

namespace lunar::app {

struct IceCandidatePayload {
    std::string candidate;
    std::string sdp_mid = "0";
    int sdp_mline_index = 0;
    std::string message_type = "iceCandidate";
};

class IceCandidateProcessor {
public:
    std::vector<IceCandidatePayload> fromLocal(
        const std::vector<webrtc::IceCandidate>& local) const;
    std::vector<IceCandidatePayload> parseRemotePayload(
        const std::string& payload,
        const StreamProfile& profile) const;
    std::string toApiJson(const std::vector<IceCandidatePayload>& candidates) const;
    std::vector<std::string> toLibPeerLines(
        const std::vector<IceCandidatePayload>& candidates) const;
};

} // namespace lunar::app
