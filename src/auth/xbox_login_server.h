#pragma once

#ifdef __SWITCH__

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace lunar::auth {

class XboxLoginServer {
public:
    XboxLoginServer() = default;
    ~XboxLoginServer();

    XboxLoginServer(const XboxLoginServer&) = delete;
    XboxLoginServer& operator=(const XboxLoginServer&) = delete;

    bool start(const std::string& verification_url, const std::string& user_code,
               const std::string& locale);
    void stop();

    std::string getHelperUrl() const;
    std::string getError() const;

private:
    void run(std::string session_path, std::string page);

    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    int listen_fd_ = -1;
    std::string helper_url_;
    std::string error_;
};

} // namespace lunar::auth

#endif
