#ifdef __SWITCH__

#include "ps_manager.h"
#include "ps_discovery.h"
#include "ps_registration.h"
#include "ps_pairing_account.h"
#include "psn_auth_utils.h"
#include "chiaki_log_adapter.h"
#include "../common.h"
#include "../diagnostics.h"
#include <borealis.hpp>
#include <cerrno>
#include <cstdlib>
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
    bool token_refreshed = false;
    if (!psn_auth_.ensureValidToken({}, &token_refreshed)) {
        psn_device_error_ = psn_auth_.getAuthError();
        return false;
    }
    if (token_refreshed && !psn_auth_.saveToken(get_psn_token_path())) {
        psn_device_error_ = "PSN session refreshed but could not be saved";
        return false;
    }

    std::string error;
    bool ok = repository_->fetchPsnDevices(
        psn_auth_.getAccessToken(), cb, &error);
    if (!ok && repository_->getLastPsnStatusCode() == 401) {
        diagnosticLog("ps-manager", "PSN device token rejected; refreshing and retrying");
        if (psn_auth_.refreshToken()) {
            if (!psn_auth_.saveToken(get_psn_token_path())) {
                error = "PSN session refreshed but could not be saved";
            } else {
                ok = repository_->fetchPsnDevices(
                    psn_auth_.getAccessToken(), std::move(cb), &error);
            }
        } else {
            error = psn_auth_.getAuthError();
        }
    }

    if (!ok) {
        psn_device_error_ = std::move(error);
        return false;
    }

    repository_->savePsnCache(psn_auth_.getAccountId());
    psn_device_error_.clear();
    return true;
}

bool PsManager::loadPsnDeviceCache() {
    const auto account_id = psn_auth_.getAccountId();
    if (account_id.empty()) return false;
    return repository_->loadPsnCache(account_id);
}

void PsManager::clearPsnDeviceCache() {
    repository_->clearPsnCache();
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

std::string PsManager::getPairingAccountId() const {
    // Local registration has its own identity. Do not silently reuse the PSN
    // account selected for remote play: it may not be the user currently
    // active on the console, which makes an otherwise correct PIN fail.
    return loadManualPsnAccountId();
}

bool PsManager::saveManualPairingAccountId(const std::string& account_id) {
    return saveManualPsnAccountId(account_id);
}

void PsManager::registerHost(const std::string& host_addr, uint32_t pin, int target,
                             const std::string& account_id, RegistrationCallback cb) {
    if (registration_) {
        registration_->stop();
        registration_.reset();
    }

    std::string account_id_bytes;
    if (target >= CHIAKI_TARGET_PS4_9 &&
        (!base64Decode(account_id, account_id_bytes) || account_id_bytes.size() != 8)) {
        auto callback = std::make_shared<RegistrationCallback>(std::move(cb));
        brls::sync([callback]() {
            if (*callback) {
                (*callback)(RegistrationResult::Failed,
                    "Local Account ID is required; use phone pairing");
            }
        });
        return;
    }

    registration_ = std::make_unique<PsRegistration>(&chiaki_log_);
    auto* result = new ChiakiRegisteredHost{};
    auto alive = std::make_shared<std::atomic<bool>>(true);

    auto callback = std::make_shared<RegistrationCallback>(std::move(cb));
    const bool started = registration_->start(host_addr, pin, target, account_id, result,
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

    const size_t key_length = strnlen(
        reinterpret_cast<const char*>(cred->rp_regist_key),
        sizeof(cred->rp_regist_key));
    if (key_length == 0 || key_length > 8) {
        cb(false, "Invalid registration key");
        return;
    }
    char key_text[9]{};
    std::memcpy(key_text, cred->rp_regist_key, key_length);
    char* key_end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(key_text, &key_end, 16);
    if (errno != 0 || key_end != key_text + key_length) {
        cb(false, "Invalid registration key");
        return;
    }
    const uint64_t user_credential = static_cast<uint64_t>(parsed);
    diagnosticLog("ps-manager", "wakeup target=%s platform=%s key_chars=%zu",
                  host_addr.c_str(), is_ps5 ? "ps5" : "ps4", key_length);

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
    else cb(false, chiaki_error_string(err));
}

ResolvedRoute PsManager::resolveRoute(const PsConsole& console) const {
    return PsConsoleResolver::resolve(console, psn_auth_.hasValidToken());
}

} // namespace lunar::ps

#endif
