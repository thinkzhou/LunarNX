#ifdef __SWITCH__

#include "ps_discovery.h"
#include "../diagnostics.h"
#include <arpa/inet.h>
#include <algorithm>
#include <cstring>
#include <switch.h>

namespace lunar::ps {
namespace {

DiscoveredConsole convertHost(ChiakiDiscoveryHost& host) {
    DiscoveredConsole console;
    if (host.host_addr) console.host_addr = host.host_addr;
    if (host.host_name) console.host_name = host.host_name;
    if (host.host_id) console.host_id = host.host_id;
    if (host.system_version) console.system_version = host.system_version;
    if (host.running_app_name) console.running_app_name = host.running_app_name;
    console.host_request_port = host.host_request_port;
    console.console_type = chiaki_discovery_host_is_ps5(&host)
        ? PsConsoleType::PS5 : PsConsoleType::PS4;
    console.target = static_cast<int>(
        chiaki_discovery_host_system_version_target(&host));
    switch (host.state) {
        case CHIAKI_DISCOVERY_HOST_STATE_READY:
            console.state = PsConsoleState::Ready;
            break;
        case CHIAKI_DISCOVERY_HOST_STATE_STANDBY:
            console.state = PsConsoleState::Standby;
            break;
        default:
            console.state = PsConsoleState::Unknown;
            break;
    }
    return console;
}

} // namespace

PsDiscovery::PsDiscovery(ChiakiLog* log) : log_(log) {}

PsDiscovery::~PsDiscovery() {
    stop();
}

bool PsDiscovery::start(const std::vector<std::string>& manual_hosts,
                        HostFoundCallback cb) {
    if (running_.load()) return false;
    callback_ = std::move(cb);

    sockaddr_in broadcast{};
    broadcast.sin_family = AF_INET;
    broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    sockaddr_storage send_addr{};
    std::memcpy(&send_addr, &broadcast, sizeof(broadcast));

    // 255.255.255.255 is filtered by some APs. Add the subnet-directed
    // broadcast used by chiaki-ng's Switch discovery implementation.
    sockaddr_storage directed_broadcast{};
    uint32_t current_ip = 0;
    uint32_t subnet_mask = 0;
    Result nifm_result = nifmInitialize(NifmServiceType_User);
    if (R_SUCCEEDED(nifm_result)) {
        nifm_result = nifmGetCurrentIpConfigInfo(
            &current_ip, &subnet_mask, nullptr, nullptr, nullptr);
        nifmExit();
    }
    if (R_SUCCEEDED(nifm_result) && subnet_mask != 0) {
        auto* directed = reinterpret_cast<sockaddr_in*>(&directed_broadcast);
        directed->sin_family = AF_INET;
        directed->sin_addr.s_addr = current_ip | ~subnet_mask;
    }

    std::vector<sockaddr_storage> periodic_targets;
    if (directed_broadcast.ss_family == AF_INET) {
        periodic_targets.push_back(directed_broadcast);
    }
    for (const auto& host : manual_hosts) {
        sockaddr_in direct{};
        direct.sin_family = AF_INET;
        if (inet_pton(AF_INET, host.c_str(), &direct.sin_addr) != 1) continue;
        sockaddr_storage target{};
        std::memcpy(&target, &direct, sizeof(direct));
        const bool duplicate = std::any_of(periodic_targets.begin(),
            periodic_targets.end(), [&](const sockaddr_storage& existing) {
                const auto* existing_addr = reinterpret_cast<const sockaddr_in*>(&existing);
                return existing.ss_family == AF_INET &&
                    existing_addr->sin_addr.s_addr == direct.sin_addr.s_addr;
            });
        if (!duplicate) periodic_targets.push_back(target);
    }

    ChiakiDiscoveryServiceOptions options{};
    options.hosts_max = 16;
    options.host_drop_pings = 3;
    options.ping_ms = 500;
    options.ping_initial_ms = 0;
    options.send_addr = &send_addr;
    options.send_addr_size = sizeof(broadcast);
    options.broadcast_addrs = periodic_targets.empty()
        ? nullptr : periodic_targets.data();
    options.broadcast_num = periodic_targets.size();
    options.cb = onHostsDiscovered;
    options.cb_user = this;

    ChiakiErrorCode err = chiaki_discovery_service_init(&service_, &options, log_);
    if (err != CHIAKI_ERR_SUCCESS) {
        callback_ = {};
        return false;
    }
    running_.store(true);

    diagnosticLog("ps-discovery",
                  "periodic_targets=%zu saved_hosts=%zu directed_broadcast=%d",
                  periodic_targets.size(), manual_hosts.size(),
                  directed_broadcast.ss_family == AF_INET ? 1 : 0);
    return true;
}

void PsDiscovery::stop() {
    if (!running_.exchange(false)) return;
    chiaki_discovery_service_fini(&service_);
}

std::vector<DiscoveredConsole> PsDiscovery::getDiscoveredHosts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hosts_;
}

void PsDiscovery::onHostsDiscovered(ChiakiDiscoveryHost* hosts, size_t count,
                                    void* user) {
    auto* self = static_cast<PsDiscovery*>(user);
    if (!self || !self->running_.load()) return;

    std::vector<DiscoveredConsole> converted;
    converted.reserve(count);
    for (size_t i = 0; i < count; ++i) converted.push_back(convertHost(hosts[i]));

    HostFoundCallback callback;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->hosts_ = converted;
        callback = self->callback_;
    }
    if (callback) callback(std::move(converted));
}

} // namespace lunar::ps

#endif
