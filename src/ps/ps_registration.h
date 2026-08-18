#pragma once

#ifdef __SWITCH__

#include <chiaki/regist.h>
#include <chiaki/log.h>
#include <atomic>
#include <functional>
#include <string>

namespace lunar::ps {

enum class RegistrationResult { Success, Failed, Cancelled };

class PsRegistration {
public:
    using ResultCallback = std::function<void(RegistrationResult, const std::string& error)>;

    explicit PsRegistration(ChiakiLog* log);
    ~PsRegistration();

    PsRegistration(const PsRegistration&) = delete;
    PsRegistration& operator=(const PsRegistration&) = delete;

    ChiakiErrorCode start(const std::string& host, uint32_t pin, int target,
                          const std::string& psn_account_id,
                          ChiakiRegisteredHost* result_out, ResultCallback cb);
    void stop();

private:
    ChiakiLog* log_;
    ChiakiLogSniffer log_sniffer_{};
    ChiakiRegist regist_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> log_sniffer_initialized_{false};
    ResultCallback callback_;
    ChiakiRegisteredHost* result_out_ = nullptr;

    static void onRegistEvent(ChiakiRegistEvent* event, void* user);
};

} // namespace lunar::ps

#endif
