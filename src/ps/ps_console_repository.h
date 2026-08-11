#pragma once

#ifdef __SWITCH__

#include "ps_console.h"
#include "psn_auth_manager.h"
#include <netinet/in.h>
#include <chiaki/log.h>
#include <chiaki/remote/holepunch.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <vector>

namespace lunar::ps {

class PsDiscovery;

class PsConsoleRepository {
public:
    using HostListCallback = std::function<void(const std::vector<PsConsole>&)>;

    PsConsoleRepository(ChiakiLog* log);
    ~PsConsoleRepository();

    PsConsoleRepository(const PsConsoleRepository&) = delete;
    PsConsoleRepository& operator=(const PsConsoleRepository&) = delete;

    // Start LAN discovery
    bool startDiscovery(HostListCallback cb);
    void stopDiscovery();

    // Fetch PSN device list (requires valid OAuth token)
    bool fetchPsnDevices(const std::string& access_token,
                         HostListCallback cb, std::string* error);
    int getLastPsnStatusCode() const { return last_psn_status_code_; }

    // Get current unified console list
    std::vector<PsConsole> getUnifiedList() const;
    void setRegisteredCredentials(std::vector<RegisteredCredential> credentials);

    // Expose discovery for wakeup
    PsDiscovery* discovery() { return discovery_.get(); }

private:
    void mergeAndNotify(HostListCallback cb);

    ChiakiLog* log_;
    std::unique_ptr<PsDiscovery> discovery_;
    std::vector<PsConsole> lan_consoles_;
    std::vector<PsConsole> psn_consoles_;
    std::vector<RegisteredCredential> credentials_;
    mutable std::mutex mutex_;
    std::atomic<bool> discovering_{false};
    std::atomic<int> last_psn_status_code_{0};
};

} // namespace lunar::ps

#endif
