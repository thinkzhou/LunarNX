#pragma once

#ifdef __SWITCH__

#include "ps_connection_plan.h"
#include "ps_connection_trace.h"
#include <netinet/in.h>
#include <chiaki/remote/holepunch.h>
#include <chiaki/log.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <memory>
#include <string>

namespace lunar::ps {

struct PsRemoteResult {
    ChiakiHolepunchSession holepunch_session = nullptr;
    std::string psn_account_id;
    std::string failed_phase;
    std::string error_detail;
    ChiakiErrorCode error = CHIAKI_ERR_SUCCESS;
    long http_status = 0;
    int attempts = 0;
    bool token_refresh_attempted = false;
    bool token_refresh_failed = false;
    bool token_refresh_session_expired = false;
    bool valid = false;
};

class PsRemoteConnector {
public:
    using StatusCallback = std::function<void(const std::string& phase)>;
    using TokenRefreshCallback =
        std::function<bool(std::string& refreshed_token, std::string& error,
                           bool& session_expired)>;

    PsRemoteConnector(const std::string& psn_token, ChiakiLog* log,
                      std::shared_ptr<PsConnectionTrace> trace);
    ~PsRemoteConnector();

    PsRemoteConnector(const PsRemoteConnector&) = delete;
    PsRemoteConnector& operator=(const PsRemoteConnector&) = delete;

    bool connect(const PsConnectionPlan& plan,
                 StatusCallback on_status,
                 TokenRefreshCallback refresh_token,
                 PsRemoteResult& result);
    void cancel();

private:
    std::string psn_token_;
    ChiakiLog* log_;
    std::shared_ptr<PsConnectionTrace> trace_;
    ChiakiHolepunchSession session_ = nullptr;
    std::mutex session_mutex_;
    std::mutex retry_mutex_;
    std::condition_variable retry_cv_;
    std::atomic<bool> cancel_requested_{false};
};

} // namespace lunar::ps

#endif
