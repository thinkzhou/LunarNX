#pragma once

#ifdef __SWITCH__

#include "ps_console.h"
#include "ps_credentials.h"
#include "ps_console_repository.h"
#include "ps_console_resolver.h"
#include "ps_registration.h"
#include "psn_auth_manager.h"
#include <chiaki/log.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lunar::ps {

class PsRegistration;

class PsManager {
public:
    using HostListCallback = std::function<void(const std::vector<PsConsole>&)>;
    using RegistrationCallback = std::function<void(RegistrationResult, const std::string&)>;
    using WakeupCallback = std::function<void(bool success, const std::string& error)>;

    PsManager();
    ~PsManager();

    PsManager(const PsManager&) = delete;
    PsManager& operator=(const PsManager&) = delete;

    // Unified discovery (LAN + PSN)
    bool startDiscovery(HostListCallback cb);
    void stopDiscovery();
    std::vector<PsConsole> getDiscoveredHosts() const;

    // PSN
    PsnAuthManager& psnAuth() { return psn_auth_; }
    bool hasPsnToken() const { return psn_auth_.hasValidToken(); }
    bool hasStoredPsnSession() const { return psn_auth_.hasStoredSession(); }
    std::string getPsnAccessToken() const { return psn_auth_.getAccessToken(); }
    std::string getPsnAccountId() const { return psn_auth_.getAccountId(); }
    std::string getPairingAccountId(const std::string& console_key = {}) const;
    bool saveManualPairingAccountId(const std::string& account_id,
                                    const std::string& console_key = {});

    // PSN device list (requires valid token)
    bool fetchPsnDevices(HostListCallback cb);
    bool loadPsnDeviceCache();
    void clearPsnDeviceCache();
    const std::string& getPsnDeviceError() const { return psn_device_error_; }

    // Credentials
    bool loadCredentials();
    bool hasCredentialsFor(const std::string& host_id) const;
    std::optional<RegisteredCredential> getCredential(const std::string& host_id) const;

    // Registration
    void registerHost(const std::string& host_addr, uint32_t pin, int target,
                      const std::string& account_id, RegistrationCallback cb);
    void cancelRegistration();

    // Wakeup
    void wakeupHost(const std::string& host_addr, const std::string& host_id,
                    bool is_ps5, WakeupCallback cb);

    // Resolve route for a console
    ResolvedRoute resolveRoute(const PsConsole& console) const;

private:
    std::string psn_device_error_;
    ChiakiLog chiaki_log_;
    PsCredentials credentials_;
    PsnAuthManager psn_auth_;
    std::unique_ptr<PsConsoleRepository> repository_;
    std::unique_ptr<PsRegistration> registration_;
    std::atomic<uint64_t> registration_generation_{0};
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> discovering_{false};
};

} // namespace lunar::ps

#endif
