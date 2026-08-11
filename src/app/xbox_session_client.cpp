#include "xbox_session_client.h"
#include "../diagnostics.h"
#include "../platform/network_worker.h"

#include <algorithm>

namespace lunar::app {
namespace {

constexpr std::chrono::milliseconds kSignalingPollInterval{1000};
constexpr int kSignalingPollAttempts = 45;

} // namespace

XboxSessionClient::XboxSessionClient(std::shared_ptr<api::XboxApiClient> api)
    : api_(std::move(api)) {}

ProvisionedSession XboxSessionClient::createAndWait(
    const StreamProfile& profile,
    const CancelCallback& cancel,
    const StatusCallback& status,
    const SleepCallback& sleep) {
    ProvisionedSession result;
    bool sent_connect = false;

    if (!api_) {
        result.error = "Xbox API client unavailable";
        lunar::diagnosticLog("xbox-session", "%s", result.error.c_str());
        return result;
    }

    if (cancel && cancel()) {
        result.status = SessionStartStatus::Cancelled;
        result.error = "Connection cancelled";
        return result;
    }

    const bool is_cloud = profile.type == SessionType::Cloud;
    api_->setSessionKind(is_cloud ? api::GssvSessionKind::Cloud
                                  : api::GssvSessionKind::Home);
    if (!profile.base_url.empty()) {
        api_->setBaseUrl(profile.base_url);
    }

    if (status) {
        status(is_cloud ? "Creating cloud session..." : "Creating session...");
    }

    api::CreateSessionRequest request;
    request.kind = is_cloud ? api::GssvSessionKind::Cloud : api::GssvSessionKind::Home;
    request.server_id = profile.server_id;
    request.title_id = profile.title_id;
    request.width = profile.width;
    request.height = profile.height;
    request.os_name = profile.os_name;
    request.locale = profile.locale;
    result.session_id = api_->createSession(request, cancel);

    if (cancel && cancel()) {
        result.status = SessionStartStatus::Cancelled;
        result.error = "Connection cancelled";
        return result;
    }
    if (result.session_id.empty()) {
        result.error = api_->getLastError().empty()
            ? "Session creation failed"
            : api_->getLastError();
        lunar::diagnosticLog("xbox-session", "Session creation failed: %s",
                             result.error.c_str());
        return result;
    }

    // Cloud may queue (WaitingForResources) for a long time; home is usually quick.
    const int max_wait_seconds = is_cloud ? 180 : 30;
    if (status) {
        status(is_cloud ? "Waiting for cloud..." : "Waiting for console (30s)...");
    }
    for (int i = 0; i < max_wait_seconds; ++i) {
        if (cancel && cancel()) {
            result.status = SessionStartStatus::Cancelled;
            result.error = "Connection cancelled";
            return result;
        }

        const std::string state = api_->pollSessionState(result.session_id, cancel);
        lunar::diagnosticLog("xbox-session", "poll state[%d]=%s session=%s",
                             i, state.c_str(), result.session_id.c_str());
        if (status && !state.empty()) {
            // Keep UI/probe informed of raw state transitions.
            status(std::string("State: ") + state);
        }
        if (cancel && cancel()) {
            result.status = SessionStartStatus::Cancelled;
            result.error = "Connection cancelled";
            return result;
        }
        if (state == "Provisioned") {
            result.config = api_->getSessionConfig(result.session_id, cancel);
            break;
        }
        if (state == "ReadyToConnect") {
            if (profile.msal_user_token.empty()) {
                result.error = "Cloud session requires MSAL user token for connect.";
                lunar::diagnosticLog("xbox-session", "%s", result.error.c_str());
                return result;
            }
            if (!sent_connect) {
                if (status) {
                    status("Authorizing cloud session...");
                }
                if (!api_->sendConnect(result.session_id, profile.msal_user_token, cancel)) {
                    result.error = api_->getLastError().empty()
                        ? "Cloud session connect failed"
                        : api_->getLastError();
                    lunar::diagnosticLog("xbox-session", "ReadyToConnect failed: %s",
                                         result.error.c_str());
                    return result;
                }
                sent_connect = true;
                lunar::diagnosticLog("xbox-session", "Cloud connect accepted; waiting for Provisioned");
            }
            // Continue polling after successful connect.
        } else if (state == "Error" || state == "Failed") {
            result.error = api_->getLastError().empty()
                ? (is_cloud ? "Cloud provisioning failed" : "Console provisioning failed")
                : api_->getLastError();
            lunar::diagnosticLog("xbox-session", "Provisioning failed: %s",
                                 result.error.c_str());
            return result;
        }
        if (status && i % 5 == 4) {
            const int remaining = max_wait_seconds - i - 1;
            status(is_cloud
                ? ("Waiting for cloud (" + std::to_string(remaining) + "s)...")
                : ("Waiting for console (" + std::to_string(remaining) + "s)..."));
        }
        if (sleep && !sleep(std::chrono::seconds(1))) {
            result.status = SessionStartStatus::Cancelled;
            result.error = "Connection cancelled";
            return result;
        }
    }

    if (cancel && cancel()) {
        result.status = SessionStartStatus::Cancelled;
        result.error = "Connection cancelled";
        return result;
    }

    if (result.config.ip_address.empty()) {
        result.config = api_->getSessionConfig(result.session_id, cancel);
    }
    if (cancel && cancel()) {
        result.status = SessionStartStatus::Cancelled;
        result.error = "Connection cancelled";
        return result;
    }
    if (result.config.ip_address.empty()) {
        const std::string api_err = api_->getLastError();
        result.error = api_err.empty()
            ? ("Session not provisioned (no server IP). session_id=" + result.session_id)
            : api_err;
        lunar::diagnosticLog("xbox-session", "Session config missing: %s",
                             result.error.c_str());
        return result;
    }

    result.status = SessionStartStatus::Ok;
    lunar::diagnosticLog("xbox-session", "Session provisioned kind=%s id=%s ip=%s port=%u",
                         is_cloud ? "cloud" : "home",
                         result.session_id.c_str(),
                         result.config.ip_address.c_str(),
                         static_cast<unsigned>(result.config.port));
    return result;
}

bool XboxSessionClient::exchangeSdpAnswer(const std::string& session_id,
                                          const std::string& offer,
                                          std::string& answer,
                                          const CancelCallback& cancel,
                                          const SleepCallback& sleep) {
    answer.clear();
    if (!api_ || offer.empty()) {
        lunar::diagnosticLog("xbox-session", "SDP exchange skipped api=%s offer_empty=%s",
                             api_ ? "true" : "false",
                             offer.empty() ? "true" : "false");
        return false;
    }
    if (!api_->sendSdpOffer(session_id, offer, cancel)) {
        lunar::diagnosticLog("xbox-session", "Send SDP offer failed: %s",
                             api_->getLastError().c_str());
        return false;
    }

    for (int i = 0; i < kSignalingPollAttempts; ++i) {
        if (cancel && cancel()) {
            return false;
        }
        answer = api_->getSdpAnswer(session_id, cancel);
        if (!answer.empty()) {
            return true;
        }
        if (cancel && cancel()) {
            return false;
        }
        if (i + 1 < kSignalingPollAttempts &&
            sleep && !sleep(kSignalingPollInterval)) {
            return false;
        }
    }
    lunar::diagnosticLog("xbox-session", "Get SDP answer timed out: %s",
                         api_->getLastError().c_str());
    return false;
}

bool XboxSessionClient::sendIceCandidates(
    const std::string& session_id,
    const std::vector<webrtc::IceCandidate>& candidates,
    const CancelCallback& cancel,
    const std::string& username_fragment) {
    if (!api_) {
        lunar::diagnosticLog("xbox-session", "Send ICE skipped: api unavailable");
        return false;
    }
    const auto payloads = ice_processor_.fromLocal(candidates, username_fragment);
    const bool ok = api_->sendIceCandidates(session_id,
                                            ice_processor_.toApiJson(payloads),
                                            cancel);
    if (!ok) {
        lunar::diagnosticLog("xbox-session", "Send ICE candidates failed: %s",
                             api_->getLastError().c_str());
    }
    return ok;
}

std::vector<IceCandidatePayload> XboxSessionClient::getIceCandidates(
    const std::string& session_id,
    const StreamProfile& profile,
    const CancelCallback& cancel,
    const SleepCallback& sleep) {
    if (!api_) {
        lunar::diagnosticLog("xbox-session", "Get ICE skipped: api unavailable");
        return {};
    }

    std::vector<std::string> raw_payloads;
    bool saw_real_candidate = false;
    int quiet_polls = 0;
    size_t candidate_count = 0;
    for (int i = 0; i < kSignalingPollAttempts; ++i) {
        if (cancel && cancel()) {
            return {};
        }

        const std::string payload = api_->getIceCandidates(session_id, cancel);
        if (!payload.empty()) {
            raw_payloads.push_back(payload);
            const bool is_real = ice_processor_.hasRealCandidate(payload);
            saw_real_candidate = saw_real_candidate || is_real;
            const bool server_done = payload.find("end-of-candidates") != std::string::npos;
            const auto accumulated =
                ice_processor_.parseRemotePayloads(raw_payloads, profile);
            const size_t next_count = accumulated.empty() ? 0 : accumulated.size() - 1;
            quiet_polls = next_count > candidate_count ? 0 : quiet_polls + 1;
            candidate_count = std::max(candidate_count, next_count);
            if (server_done || (saw_real_candidate && quiet_polls >= 4)) {
                break;
            }
        } else if (saw_real_candidate) {
            ++quiet_polls;
            if (quiet_polls >= 4) {
                break;
            }
        }

        if (cancel && cancel()) {
            return {};
        }
        if (i + 1 < kSignalingPollAttempts &&
            sleep && !sleep(kSignalingPollInterval)) {
            return {};
        }
    }

    auto candidates = ice_processor_.parseRemotePayloads(raw_payloads, profile);
    if (!candidates.empty()) {
        lunar::diagnosticLog("xbox-session", "Get ICE candidates succeeded count=%zu payloads=%zu real=%s",
                             candidates.size(), raw_payloads.size(),
                             saw_real_candidate ? "true" : "false");
        return candidates;
    }

    lunar::diagnosticLog("xbox-session", "Get ICE candidates timed out: %s",
                         api_->getLastError().c_str());
    return {};
}

bool XboxSessionClient::cleanupStaleSessions(const StreamProfile& profile,
                                              const CancelCallback& cancel) {
    if (!api_) {
        return false;
    }
    api_->setSessionKind(profile.type == SessionType::Cloud
                             ? api::GssvSessionKind::Cloud
                             : api::GssvSessionKind::Home);
    if (!profile.base_url.empty()) {
        api_->setBaseUrl(profile.base_url);
    }
    return api_->cleanupActiveSessions(profile.type == SessionType::Cloud
                                           ? api::GssvSessionKind::Cloud
                                           : api::GssvSessionKind::Home,
                                       cancel);
}

bool XboxSessionClient::keepAlive(const std::string& session_id,
                                  const CancelCallback& cancel) {
    return api_ && api_->sendKeepAlive(session_id, cancel);
}

void XboxSessionClient::updateTokens(const std::string& web_token,
                                     const std::string& gssv_token) {
    if (api_) {
        api_->updateTokens(web_token, gssv_token);
    }
}

void XboxSessionClient::deleteSessionAsync(const std::string& session_id) {
    if (!api_ || session_id.empty()) {
        return;
    }
    auto api = api_;
    lunar::platform::startNetworkWorker("delete-session", [api, session_id]() {
        lunar::diagnosticLog("xbox-session", "Delete session begin id=%s", session_id.c_str());
        const bool ok = api->deleteSession(session_id);
        lunar::diagnosticLog("xbox-session", "Delete session done id=%s ok=%s",
                             session_id.c_str(), ok ? "true" : "false");
    });
}

} // namespace lunar::app
