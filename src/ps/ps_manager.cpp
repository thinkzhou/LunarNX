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
#include <chrono>
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
    alive_->store(false);
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
    const auto ticket = psn_device_generation_.invalidate();
    auto cancelled = [this, ticket]() {
        return !psn_device_generation_.isCurrent(ticket);
    };
    bool token_refreshed = false;
    if (!psn_auth_.ensureValidToken({}, &token_refreshed, cancelled)) {
        if (cancelled()) return true;
        psn_device_error_ = psn_auth_.getAuthError();
        return false;
    }
    const std::string account_id = psn_auth_.getAccountId();
    if (account_id.empty()) {
        psn_device_error_ = "PSN account ID is unavailable";
        return false;
    }
    if (token_refreshed && !psn_auth_.saveToken(get_psn_token_path())) {
        if (cancelled()) return true;
        psn_device_error_ = "PSN session refreshed but could not be saved";
        return false;
    }

    std::string error;
    HostListCallback guarded_cb = [this, ticket, cb = std::move(cb)](
        const std::vector<PsConsole>& hosts) {
        if (psn_device_generation_.isCurrent(ticket) && cb) cb(hosts);
    };
    bool ok = repository_->fetchPsnDevices(
        psn_auth_.getAccessToken(), guarded_cb, &error, cancelled);
    if (cancelled()) return true;
    if (!ok && repository_->getLastPsnStatusCode() == 401) {
        diagnosticLog("ps-manager", "PSN device token rejected; refreshing and retrying");
        if (psn_auth_.refreshToken({}, cancelled)) {
            if (!psn_auth_.saveToken(get_psn_token_path())) {
                if (cancelled()) return true;
                error = "PSN session refreshed but could not be saved";
            } else {
                ok = repository_->fetchPsnDevices(
                    psn_auth_.getAccessToken(), std::move(guarded_cb), &error,
                    cancelled);
            }
        } else {
            if (cancelled()) return true;
            error = psn_auth_.getAuthError();
        }
    }
    if (cancelled()) return true;

    if (!ok) {
        psn_device_error_ = std::move(error);
        return false;
    }

    if (!repository_->savePsnCache(account_id, cancelled)) {
        if (cancelled()) return true;
        // The live list is already usable. Treat persistence as best-effort so
        // a transient SD-card error does not hide otherwise valid consoles.
        diagnosticLog("ps-manager", "PSN console cache save failed");
    }
    psn_device_error_.clear();
    return true;
}

bool PsManager::loadPsnDeviceCache() {
    psn_device_generation_.invalidate();
    const auto account_id = psn_auth_.getAccountId();
    if (account_id.empty()) return false;
    return repository_->loadPsnCache(account_id);
}

void PsManager::clearPsnDeviceCache() {
    psn_device_generation_.invalidate();
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

std::string PsManager::getPairingAccountId(const std::string& console_key) const {
    // Local registration has its own identity. Do not silently reuse the PSN
    // account selected for remote play: it may not be the user currently
    // active on the console, which makes an otherwise correct PIN fail.
    return loadManualPsnAccountId(console_key);
}

bool PsManager::saveManualPairingAccountId(const std::string& account_id,
                                           const std::string& console_key) {
    return saveManualPsnAccountId(account_id, console_key);
}

void PsManager::registerHost(const std::string& host_addr, uint32_t pin, int target,
                             const std::string& account_id, RegistrationCallback cb) {
    const uint64_t generation = registration_generation_.fetch_add(1) + 1;
    if (registration_) {
        registration_->stop();
        registration_.reset();
    }

    if (target >= CHIAKI_TARGET_PS4_9 && account_id.empty()) {
        persistentEventLog("ps-registration",
            "failed stage=missing-account-id target=%d", target);
        auto callback = std::make_shared<RegistrationCallback>(std::move(cb));
        brls::sync([this, callback, alive = alive_, generation]() {
            if (!alive->load() ||
                registration_generation_.load() != generation) return;
            if (*callback) {
                (*callback)(RegistrationResult::Failed,
                    "Local Account ID is required; use phone pairing");
            }
        });
        return;
    }

    registration_ = std::make_unique<PsRegistration>(&chiaki_log_);
    auto result = std::make_shared<ChiakiRegisteredHost>();

    auto callback = std::make_shared<RegistrationCallback>(std::move(cb));
    const ChiakiErrorCode start_error = registration_->start(
        host_addr, pin, target, account_id, result.get(),
        [this, callback, result, alive = alive_, host_addr, account_id, target, generation](
            RegistrationResult res, const std::string& err) {
            brls::sync([this, callback, result, alive, host_addr, account_id, target,
                        generation, res, err]() {
                if (!alive->load() ||
                    registration_generation_.load() != generation) return;

                // Registration has finished on the Chiaki worker. Finalize it
                // here, from the UI thread, before exposing the result or
                // starting a stream. Calling fini in the Chiaki callback would
                // attempt to join the current thread.
                if (registration_) {
                    registration_->stop();
                    registration_.reset();
                }

                RegistrationResult final_result = res;
                std::string final_error = err;
                if (res == RegistrationResult::Success) {
                    RegisteredCredential cred;
                    cred.server_mac = macFromBytes(result->server_mac);
                    cred.last_known_addr = host_addr;
                    if (result->server_nickname[0]) {
                        cred.nickname = result->server_nickname;
                    }
                    cred.target = target;
                    std::memcpy(cred.rp_regist_key, result->rp_regist_key,
                                sizeof(cred.rp_regist_key));
                    cred.rp_key_type = result->rp_key_type;
                    std::memcpy(cred.rp_key, result->rp_key, sizeof(cred.rp_key));

                    // Persist the identity only after the console accepted it.
                    // Keep both the stable MAC and the address used for manual
                    // pairing so either discovery path resolves the same user.
                    saveManualPsnAccountId(account_id, cred.server_mac);
                    saveManualPsnAccountId(account_id, host_addr);

                    if (!credentials_.addAndSave(cred, get_ps_credentials_path())) {
                        persistentEventLog("ps-registration",
                            "failed stage=credential-save target=%d", target);
                        final_result = RegistrationResult::Failed;
                        final_error =
                            "Pairing completed, but credentials could not be saved";
                    } else {
                        repository_->setRegisteredCredentials(
                            credentials_.getRegisteredHosts());
                        repository_->notePairedLocalHost(cred);
                    }
                }

                if (*callback) (*callback)(final_result, final_error);
            });
        });
    if (start_error != CHIAKI_ERR_SUCCESS) {
        registration_.reset();
        const std::string error = "Could not start local pairing: " +
            std::string(chiaki_error_string(start_error));
        brls::sync([this, callback, alive = alive_, generation, error]() {
            if (!alive->load() ||
                registration_generation_.load() != generation) return;
            if (*callback) (*callback)(RegistrationResult::Failed, error);
        });
    }
}

void PsManager::cancelRegistration() {
    registration_generation_.fetch_add(1);
    if (registration_) {
        registration_->stop();
        registration_.reset();
    }
}

void PsManager::wakeupHost(const std::string& host_addr, const std::string& host_id,
                            bool is_ps5, WakeupCallback cb) {
    const auto wake_started = std::chrono::steady_clock::now();
    auto cred = credentials_.findByMac(host_id);
    if (!cred) {
        persistentEventLog("ps-wakeup",
            "stage=credentials outcome=missing platform=%s host_present=%d",
            is_ps5 ? "ps5" : "ps4", host_addr.empty() ? 0 : 1);
        cb(false, "Host not paired");
        return;
    }

    const size_t key_length = strnlen(
        reinterpret_cast<const char*>(cred->rp_regist_key),
        sizeof(cred->rp_regist_key));
    if (key_length == 0 || key_length > 8) {
        persistentEventLog("ps-wakeup",
            "stage=credentials outcome=invalid platform=%s key_chars=%zu",
            is_ps5 ? "ps5" : "ps4", key_length);
        cb(false, "Invalid registration key");
        return;
    }
    char key_text[9]{};
    std::memcpy(key_text, cred->rp_regist_key, key_length);
    char* key_end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(key_text, &key_end, 16);
    if (errno != 0 || key_end != key_text + key_length) {
        persistentEventLog("ps-wakeup",
            "stage=credentials outcome=parse-failed platform=%s key_chars=%zu",
            is_ps5 ? "ps5" : "ps4", key_length);
        cb(false, "Invalid registration key");
        return;
    }
    const uint64_t user_credential = static_cast<uint64_t>(parsed);
    diagnosticLog("ps-manager", "wakeup target=%s platform=%s key_chars=%zu",
                  host_addr.c_str(), is_ps5 ? "ps5" : "ps4", key_length);

    ChiakiDiscovery discovery{};
    ChiakiErrorCode err = chiaki_discovery_init(&discovery, &chiaki_log_, AF_INET);
    if (err != CHIAKI_ERR_SUCCESS) {
        persistentEventLog("ps-wakeup",
            "stage=discovery-init outcome=failed platform=%s error=%d error_name=%s",
            is_ps5 ? "ps5" : "ps4", static_cast<int>(err),
            chiaki_error_string(err));
        cb(false, "Discovery init failed");
        return;
    }

    err = chiaki_discovery_wakeup(&chiaki_log_, &discovery, host_addr.c_str(),
                                   user_credential, is_ps5);
    chiaki_discovery_fini(&discovery);

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - wake_started).count();
    persistentEventLog("ps-wakeup",
        "stage=wakeup-send outcome=%s platform=%s elapsed_ms=%lld error=%d error_name=%s",
        err == CHIAKI_ERR_SUCCESS ? "ok" : "failed",
        is_ps5 ? "ps5" : "ps4", static_cast<long long>(elapsed_ms),
        static_cast<int>(err), chiaki_error_string(err));

    if (err == CHIAKI_ERR_SUCCESS) cb(true, "");
    else cb(false, chiaki_error_string(err));
}

PsConnectionPlan PsManager::planConnection(
    const PsConsole& console,
    PsConnectionPreference preference) const {
    return PsConnectionPlanner::makePlan(
        console, psn_auth_.hasStoredSession(), preference);
}

} // namespace lunar::ps

#endif
