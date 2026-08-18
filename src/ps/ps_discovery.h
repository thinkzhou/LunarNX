#pragma once

#ifdef __SWITCH__

#include "ps_console.h"
#include <chiaki/discoveryservice.h>
#include <chiaki/log.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace lunar::ps {

class PsDiscovery {
public:
    using HostFoundCallback = std::function<void(std::vector<DiscoveredConsole>)>;

    explicit PsDiscovery(ChiakiLog* log);
    ~PsDiscovery();

    PsDiscovery(const PsDiscovery&) = delete;
    PsDiscovery& operator=(const PsDiscovery&) = delete;

    bool start(const std::vector<std::string>& manual_hosts,
               HostFoundCallback cb);
    void stop();
    std::vector<DiscoveredConsole> getDiscoveredHosts() const;

private:
    ChiakiLog* log_;
    ChiakiDiscoveryService service_{};
    std::atomic<bool> running_{false};
    HostFoundCallback callback_;
    std::vector<DiscoveredConsole> hosts_;
    mutable std::mutex mutex_;

    static void onHostsDiscovered(ChiakiDiscoveryHost* hosts, size_t count, void* user);
};

} // namespace lunar::ps

#endif
