#pragma once

#ifdef __SWITCH__

#include <netinet/in.h>
#include <chiaki/remote/holepunch.h>
#include <chiaki/log.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

namespace lunar::ps {

struct PsRemoteResult {
    ChiakiHolepunchSession holepunch_session = nullptr;
    std::string psn_account_id;
    std::string failed_phase;
    ChiakiErrorCode error = CHIAKI_ERR_SUCCESS;
    int attempts = 0;
    bool valid = false;
};

class PsRemoteConnector {
public:
    using StatusCallback = std::function<void(const std::string& phase)>;

    PsRemoteConnector(const std::string& psn_token, ChiakiLog* log);
    ~PsRemoteConnector();

    PsRemoteConnector(const PsRemoteConnector&) = delete;
    PsRemoteConnector& operator=(const PsRemoteConnector&) = delete;

    bool connect(int console_type, const uint8_t* console_uid,
                 StatusCallback on_status, PsRemoteResult& result);
    void cancel();

private:
    std::string psn_token_;
    ChiakiLog* log_;
    ChiakiHolepunchSession session_ = nullptr;
    std::mutex session_mutex_;
    std::mutex retry_mutex_;
    std::condition_variable retry_cv_;
    std::atomic<bool> cancel_requested_{false};
};

} // namespace lunar::ps

#endif
