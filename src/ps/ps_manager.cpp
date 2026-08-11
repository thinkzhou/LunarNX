#ifdef __SWITCH__

#include "ps_manager.h"
#include "ps_discovery.h"
#include "ps_registration.h"
#include "chiaki_log_adapter.h"
#include "../common.h"
#include "../diagnostics.h"
#include <borealis.hpp>
#include <cstring>

namespace lunar::ps {

PsManager::PsManager()
    : chiaki_log_(makeChiakiDiagnosticLog("chiaki-ps")) {
    diagnosticLog("ps-manager", "construct");
    chiaki_lib_init();
    diagnosticLog("ps-manager", "chiaki_lib_init ok");
    repository_ = std::make_unique<PsConsoleRepository>(&chiaki_log_);
    diagnosticLog("ps-manager", "repository ok");
}

PsManager::~PsManager() {
    stopDiscovery();
    cancelRegistration();
}

bool PsManager::startDiscovery(HostListCallback cb) {
    diagnosticLog("ps-manager", "startDiscovery");
    const bool ok = repository_->startDiscovery(std::move(cb));
    diagnosticLog("ps-manager", "startDiscovery ok=%d", ok ? 1 : 0);
    return ok;
}

void PsManager::stopDiscovery() {
    repository_->stopDiscovery();
}

std::vector<PsConsole> PsManager::getDiscoveredHosts() const {
    return repository_->getUnifiedList();
}

bool PsManager::fetchPsnDevices(HostListCallback cb) {
    if (!psn_auth_.ensureValidToken()) return false;

    std::string error;
    bool ok = repository_->fetchPsnDevices(
        psn_auth_.getAccessToken(), cb, &error);
    if (!ok && repository_->getLastPsnStatusCode() == 401) {
        diagnosticLog("ps-manager", "PSN device token rejected; refreshing and retrying");
        if (psn_auth_.refreshToken()) {
            ok = repository_->fetchPsnDevices(
                psn_auth_.getAccessToken(), std::move(cb), &error);
        } else {
            error = psn_auth_.getAuthError();
        }
    }

    if (!ok) {
        psn_device_error_ = std::move(error);
        return false;
    }

    psn_auth_.saveToken(get_psn_token_path());
    psn_device_error_.clear();
    return true;
}

bool PsManager::loadCredentials() {
    bool loaded = credentials_.load(get_ps_credentials_path());
    repository_->setRegisteredCredentials(credentials_.getRegisteredHosts());
    return loaded;
}

bool PsManager::hasCredentialsFor(const std::string& host_id) const {
    return credentials_.findByMac(host_id).has_value();
}

std::optional<RegisteredCredential> PsManager::getCredential(
    const std::string& host_id) const {
    return credentials_.findByMac(host_id);
}

void PsManager::registerHost(const std::string& host_addr, uint32_t pin, int target,
                              RegistrationCallback cb) {
    if (registration_) {
        registration_->stop();
        registration_.reset();
    }

    registration_ = std::make_unique<PsRegistration>(&chiaki_log_);
    auto* result = new ChiakiRegisteredHost{};
    auto alive = std::make_shared<std::atomic<bool>>(true);

    auto callback = std::make_shared<RegistrationCallback>(std::move(cb));
    const bool started = registration_->start(host_addr, pin, target,
        psn_auth_.getAccountId(), result,
        [this, callback, result, alive, host_addr, pin, target](
            RegistrationResult res, const std::string& err) {
            if (!alive->load()) { delete result; return; }

            if (res == RegistrationResult::Success) {
                RegisteredCredential cred;
                cred.server_mac = macFromBytes(result->server_mac);
                cred.last_known_addr = host_addr;
                if (result->server_nickname[0]) {
                    cred.nickname = result->server_nickname;
                }
                cred.target = target;
                std::memcpy(cred.rp_regist_key, result->rp_regist_key, sizeof(cred.rp_regist_key));
                cred.rp_key_type = result->rp_key_type;
                std::memcpy(cred.rp_key, result->rp_key, sizeof(cred.rp_key));

                credentials_.add(cred);
                credentials_.save(get_ps_credentials_path());
                repository_->setRegisteredCredentials(credentials_.getRegisteredHosts());
            }

            delete result;

            brls::sync([callback, res, err]() {
                if (*callback) (*callback)(res, err);
            });
        });
    if (!started) {
        delete result;
        registration_.reset();
        brls::sync([callback]() {
            if (*callback) (*callback)(RegistrationResult::Failed,
                                       "Could not start local pairing");
        });
    }
}

void PsManager::cancelRegistration() {
    if (registration_) {
        registration_->stop();
        registration_.reset();
    }
}

void PsManager::wakeupHost(const std::string& host_addr, const std::string& host_id,
                            bool is_ps5, WakeupCallback cb) {
    auto cred = credentials_.findByMac(host_id);
    if (!cred) {
        cb(false, "Host not paired");
        return;
    }

    uint64_t user_credential = 0;
    std::memcpy(&user_credential, cred->rp_regist_key, sizeof(user_credential));

    ChiakiDiscovery discovery{};
    ChiakiErrorCode err = chiaki_discovery_init(&discovery, &chiaki_log_, AF_INET);
    if (err != CHIAKI_ERR_SUCCESS) {
        cb(false, "Discovery init failed");
        return;
    }

    err = chiaki_discovery_wakeup(&chiaki_log_, &discovery, host_addr.c_str(),
                                   user_credential, is_ps5);
    chiaki_discovery_fini(&discovery);

    if (err == CHIAKI_ERR_SUCCESS) cb(true, "");
    else cb(false, "Wakeup failed");
}

ResolvedRoute PsManager::resolveRoute(const PsConsole& console) const {
    return PsConsoleResolver::resolve(console, psn_auth_.hasValidToken());
}

} // namespace lunar::ps

#endif
