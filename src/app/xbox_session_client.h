#pragma once

#include "../api/xbox_api_client.h"
#include "../webrtc/peer_manager.h"
#include "ice_candidate_processor.h"
#include "stream_profile.h"
#include "web_rtc_transport.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lunar::app {

enum class SessionStartStatus {
    Ok,
    Cancelled,
    Unsupported,
    Failed,
};

struct ProvisionedSession {
    SessionStartStatus status = SessionStartStatus::Failed;
    std::string session_id;
    api::SessionConfig config;
    std::string error;
};

class XboxSessionClient {
public:
    using StatusCallback = std::function<void(const std::string&)>;
    using SleepCallback = std::function<bool(std::chrono::milliseconds)>;

    explicit XboxSessionClient(std::shared_ptr<api::XboxApiClient> api);

    ProvisionedSession createAndWait(const StreamProfile& profile,
                                     const CancelCallback& cancel,
                                     const StatusCallback& status,
                                     const SleepCallback& sleep);

    bool exchangeSdpAnswer(const std::string& session_id,
                           const std::string& offer,
                           std::string& answer,
                           const CancelCallback& cancel,
                           const SleepCallback& sleep);
    bool sendIceCandidates(const std::string& session_id,
                           const std::vector<webrtc::IceCandidate>& candidates,
                           const CancelCallback& cancel);
    std::vector<IceCandidatePayload> getIceCandidates(
        const std::string& session_id,
        const StreamProfile& profile,
        const CancelCallback& cancel,
        const SleepCallback& sleep);

    bool keepAlive(const std::string& session_id, const CancelCallback& cancel);
    void updateTokens(const std::string& web_token, const std::string& gssv_token);
    void deleteSessionAsync(const std::string& session_id);
    std::shared_ptr<api::XboxApiClient> api() const { return api_; }

private:
    std::shared_ptr<api::XboxApiClient> api_;
    IceCandidateProcessor ice_processor_;
};

} // namespace lunar::app
