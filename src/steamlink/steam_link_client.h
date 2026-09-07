#pragma once
#ifdef __SWITCH__

extern "C" {
#include <ihslib/client.h>
}

#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace lunar::steamlink {

struct SteamLinkHost {
    uint64_t client_id = 0;
    uint64_t instance_id = 0;
    std::string hostname;
    std::string address;
    uint16_t port = 0;
    int ostype = 0;
    bool games_running = false;
};

class SteamLinkClient {
public:
    struct Identity {
        uint64_t device_id = 0;
        std::array<uint8_t, 32> secret_key{};
    };

    using HostCallback = std::function<void(const std::vector<SteamLinkHost>&)>;
    using AuthorizationCallback = std::function<void(bool, const std::string&)>;

    SteamLinkClient();
    ~SteamLinkClient();

    SteamLinkClient(const SteamLinkClient&) = delete;
    SteamLinkClient& operator=(const SteamLinkClient&) = delete;

    bool startDiscovery(HostCallback callback);
    void stopDiscovery();
    bool authorize(const SteamLinkHost& host, const std::string& pin,
                   AuthorizationCallback callback);
    void cancelAuthorization();
    bool isAuthorized(uint64_t client_id) const;
    const std::string& lastError() const { return last_error_; }

private:
    static void onDiscovered(IHS_Client*, const IHS_HostInfo*, void* context);
    static void onAuthorizationProgress(IHS_Client*, const IHS_HostInfo*, void* context);
    static void onAuthorizationSuccess(IHS_Client*, const IHS_HostInfo*, uint64_t steam_id,
                                       void* context);
    static void onAuthorizationFailed(IHS_Client*, const IHS_HostInfo*,
                                      IHS_AuthorizationResult result, void* context);

    bool loadOrCreateIdentity();
    bool ensureClient();
    bool findHost(uint64_t client_id, IHS_HostInfo* result) const;
    void updateHost(const IHS_HostInfo& host);
    void emitHosts();
    static std::string authorizationError(IHS_AuthorizationResult result);

    Identity identity_;
    IHS_Client* client_ = nullptr;
    bool ihs_initialized_ = false;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, IHS_HostInfo> host_infos_;
    std::vector<SteamLinkHost> hosts_;
    HostCallback host_callback_;
    AuthorizationCallback authorization_callback_;
    uint64_t authorized_client_id_ = 0;
    std::string last_error_;
};

} // namespace lunar::steamlink
#endif
