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
    std::string username_fragment;
    std::string message_type = "iceCandidate";
};

class IceCandidateProcessor {
public:
    static std::string usernameFragmentFromSdp(const std::string& sdp);
    std::vector<IceCandidatePayload> fromLocal(
        const std::vector<webrtc::IceCandidate>& local,
        const std::string& username_fragment = {}) const;
    std::vector<IceCandidatePayload> parseRemotePayload(
        const std::string& payload,
        const StreamProfile& profile) const;
    std::vector<IceCandidatePayload> parseRemotePayloads(
        const std::vector<std::string>& payloads,
        const StreamProfile& profile) const;
    bool hasRealCandidate(const std::string& payload) const;
    std::string toApiJson(const std::vector<IceCandidatePayload>& candidates) const;
    std::vector<std::string> toLibPeerLines(
        const std::vector<IceCandidatePayload>& candidates) const;
};

} // namespace lunar::app
