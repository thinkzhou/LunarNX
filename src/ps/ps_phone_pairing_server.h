#pragma once

#ifdef __SWITCH__

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace lunar::ps {

struct PsPhonePairingInput {
    std::string account_id;
    uint32_t pin = 0;
};

class PsPhonePairingServer {
public:
    using Callback = std::function<void(PsPhonePairingInput)>;

    ~PsPhonePairingServer();
    bool start(const std::string& locale, Callback callback);
    void stop();
    std::string getHelperUrl() const;
    std::string getError() const;

private:
    void run(std::string session_path, std::string locale, Callback callback);

    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    int listen_fd_ = -1;
    std::string helper_url_;
    std::string error_;
};

} // namespace lunar::ps

#endif
