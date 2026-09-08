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
#include <thread>
#include <condition_variable>

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

struct SteamLinkStreamInfo {
    IHS_SocketAddress address{};
    std::array<uint8_t, 32> session_key{};
    size_t session_key_len = 0;
    uint64_t steam_id = 0;
};

class SteamLinkClient {
public:
    struct Identity {
        uint64_t device_id = 0;
        std::array<uint8_t, 32> secret_key{};
    };

    using HostCallback = std::function<void(const std::vector<SteamLinkHost>&)>;
    using AuthorizationCallback = std::function<void(bool, const std::string&)>;
    using StreamingCallback = std::function<void(bool, const SteamLinkStreamInfo&,
                                                  const std::string&)>;

    SteamLinkClient();
    ~SteamLinkClient();

    SteamLinkClient(const SteamLinkClient&) = delete;
    SteamLinkClient& operator=(const SteamLinkClient&) = delete;

    bool startDiscovery(HostCallback callback);
    void stopDiscovery();
    bool discoverAddress(const std::string& address);
    bool authorize(const SteamLinkHost& host, const std::string& pin,
                   AuthorizationCallback callback);
    bool requestStreaming(const SteamLinkHost& host, const std::string& pin,
                          int width, int height, StreamingCallback callback);
    void cancelAuthorization();
    void cancelStreaming();
    bool isAuthorized(uint64_t client_id) const;
    uint64_t authorizedSteamId(uint64_t client_id) const;
    bool getSessionClientConfig(IHS_ClientConfig* config) const;
    static void logFunction(IHS_LogLevel level, const char* tag, const char* message);
    const std::string& lastError() const { return last_error_; }

private:
    static void onDiscovered(IHS_Client*, const IHS_HostInfo*, void* context);
    static void onAuthorizationProgress(IHS_Client*, const IHS_HostInfo*, void* context);
    static void onAuthorizationSuccess(IHS_Client*, const IHS_HostInfo*, uint64_t steam_id,
                                       void* context);
    static void onAuthorizationFailed(IHS_Client*, const IHS_HostInfo*,
                                      IHS_AuthorizationResult result, void* context);
    static void onStreamingProgress(IHS_Client*, const IHS_HostInfo*, void* context);
    static void onStreamingSuccess(IHS_Client*, const IHS_HostInfo*,
                                   const IHS_SocketAddress*, const uint8_t*, size_t,
                                   void* context);
    static void onStreamingFailed(IHS_Client*, const IHS_HostInfo*,
                                  IHS_StreamingResult result, void* context);

    bool loadOrCreateIdentity();
    bool ensureClient();
    bool findHost(uint64_t client_id, IHS_HostInfo* result) const;
    void updateHost(const IHS_HostInfo& host);
    void emitHosts();
    static std::string authorizationError(IHS_AuthorizationResult result);
    static std::string streamingError(IHS_StreamingResult result);

    Identity identity_;
    IHS_Client* client_ = nullptr;
    bool ihs_initialized_ = false;
    bool discovery_started_ = false; // Discovery lifecycle is owned by the UI thread.
    mutable std::mutex mutex_;
    std::mutex streaming_operation_mutex_;
    std::mutex authorization_operation_mutex_;
    std::mutex host_publish_mutex_;
    std::mutex discovery_wait_mutex_;
    std::condition_variable discovery_wait_;
    std::thread discovery_worker_;
    bool discovery_stop_ = false;
    std::string manual_address_;
    std::unordered_map<uint64_t, uint64_t> last_seen_ms_;
    std::unordered_map<uint64_t, IHS_HostInfo> host_infos_;
    std::vector<SteamLinkHost> hosts_;
    HostCallback host_callback_;
    AuthorizationCallback authorization_callback_;
    StreamingCallback streaming_callback_;
    std::unordered_map<uint64_t, uint64_t> authorized_steam_ids_;
    std::unordered_map<uint64_t, IHS_HostInfo> authorized_hosts_;
    std::string last_error_;
};

} // namespace lunar::steamlink
#endif
