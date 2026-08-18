#ifdef __SWITCH__

#include "ps_console_repository.h"
#include "ps_discovery.h"
#include "../api/http_client.h"
#include "../common.h"
#include "../diagnostics.h"
#include <borealis.hpp>
#include <cJSON.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace lunar::ps {

PsConsoleRepository::PsConsoleRepository(ChiakiLog* log) : log_(log) {}

PsConsoleRepository::~PsConsoleRepository() {
    stopDiscovery();
}

bool PsConsoleRepository::loadPsnCache(const std::string& account_id) {
    constexpr long kMaxCacheBytes = 1024 * 1024;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        psn_consoles_.clear();
    }
    if (account_id.empty()) return false;
    FILE* file = std::fopen(lunar::get_ps_console_cache_path(), "rb");
    if (!file) return false;
    if (std::fseek(file, 0, SEEK_END) != 0) { std::fclose(file); return false; }
    const long length = std::ftell(file);
    if (length <= 0 || length > kMaxCacheBytes) { std::fclose(file); return false; }
    std::rewind(file);
    std::string content(static_cast<size_t>(length), '\0');
    const bool read_ok = std::fread(content.data(), 1, content.size(), file) == content.size();
    std::fclose(file);
    if (!read_ok) return false;

    cJSON* root = cJSON_Parse(content.c_str());
    cJSON* account = root ? cJSON_GetObjectItemCaseSensitive(root, "account_id") : nullptr;
    cJSON* consoles = root ? cJSON_GetObjectItemCaseSensitive(root, "consoles") : nullptr;
    if (!root || !cJSON_IsString(account) || account_id != account->valuestring ||
        !cJSON_IsArray(consoles)) {
        if (root) cJSON_Delete(root);
        return false;
    }

    std::vector<PsConsole> cached;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, consoles) {
        cJSON* duid = cJSON_GetObjectItemCaseSensitive(item, "duid");
        cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON* enabled = cJSON_GetObjectItemCaseSensitive(item, "remoteplay_enabled");
        if (!cJSON_IsString(duid) || !normalizeDuid(duid->valuestring) ||
            !cJSON_IsString(name) || !cJSON_IsBool(enabled)) continue;
        PsConsole console;
        console.psn_duid = *normalizeDuid(duid->valuestring);
        console.stable_id = "duid:" + console.psn_duid;
        console.nickname = name->valuestring;
        console.target = CHIAKI_TARGET_PS5_1;
        console.remote = PsRemoteEndpoint{console.psn_duid, console.nickname,
                                           cJSON_IsTrue(enabled)};
        cached.push_back(std::move(console));
    }
    cJSON_Delete(root);
    if (cached.empty()) return false;
    const size_t cached_count = cached.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        psn_consoles_ = std::move(cached);
    }
    diagnosticLog("ps-console-repository", "Loaded PSN device cache count=%zu",
                  cached_count);
    return true;
}

bool PsConsoleRepository::savePsnCache(const std::string& account_id) const {
    if (account_id.empty()) return false;
    std::vector<PsConsole> consoles;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        consoles = psn_consoles_;
    }
    if (consoles.empty()) {
        std::remove(lunar::get_ps_console_cache_path());
        return true;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "account_id", account_id.c_str());
    cJSON* array = cJSON_CreateArray();
    for (const auto& console : consoles) {
        if (console.psn_duid.empty() || !console.remote.has_value()) continue;
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "duid", console.psn_duid.c_str());
        cJSON_AddStringToObject(item, "name", console.nickname.c_str());
        cJSON_AddBoolToObject(item, "remoteplay_enabled", console.remote->remoteplay_enabled);
        cJSON_AddItemToArray(array, item);
    }
    cJSON_AddItemToObject(root, "consoles", array);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;
    const std::string path = lunar::get_ps_console_cache_path();
    const std::string temp_path = path + ".tmp";
    FILE* file = std::fopen(temp_path.c_str(), "wb");
    if (!file) { std::free(json); return false; }
    bool ok = std::fputs(json, file) >= 0;
    ok = std::fflush(file) == 0 && ok;
    ok = std::fclose(file) == 0 && ok;
    std::free(json);
    if (!ok) { std::remove(temp_path.c_str()); return false; }
    const std::string backup_path = path + ".bak";
    std::remove(backup_path.c_str());
    errno = 0;
    const bool had_existing = std::rename(path.c_str(), backup_path.c_str()) == 0;
    if (!had_existing && errno != ENOENT) {
        std::remove(temp_path.c_str());
        return false;
    }
    if (std::rename(temp_path.c_str(), path.c_str()) != 0) {
        if (had_existing) std::rename(backup_path.c_str(), path.c_str());
        std::remove(temp_path.c_str());
        return false;
    }
    if (had_existing) std::remove(backup_path.c_str());
    diagnosticLog("ps-console-repository", "Saved PSN device cache count=%zu", consoles.size());
    return true;
}

void PsConsoleRepository::clearPsnCache() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        psn_consoles_.clear();
    }
    std::remove(lunar::get_ps_console_cache_path());
    std::remove((std::string(lunar::get_ps_console_cache_path()) + ".tmp").c_str());
    std::remove((std::string(lunar::get_ps_console_cache_path()) + ".bak").c_str());
}

bool PsConsoleRepository::startDiscovery(HostListCallback cb) {
    if (discovering_.load()) return true;

    std::vector<std::string> manual_hosts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& credential : credentials_) {
            if (credential.last_known_addr.empty() ||
                std::find(manual_hosts.begin(), manual_hosts.end(),
                          credential.last_known_addr) != manual_hosts.end()) {
                continue;
            }
            manual_hosts.push_back(credential.last_known_addr);
        }
    }

    discovery_ = std::make_unique<PsDiscovery>(log_);
    bool ok = discovery_->start(manual_hosts, [this, cb = std::move(cb)](
        std::vector<DiscoveredConsole> discovered) {
        std::vector<PsConsole> consoles;
        consoles.reserve(discovered.size());
        for (const auto& raw : discovered) {
            PsConsole c;
            auto mac = normalizeMac(raw.host_id);
            if (mac) {
                c.server_mac = *mac;
                c.stable_id = "mac:" + *mac;
            } else {
                c.stable_id = "lan:" + raw.host_id;
            }
            c.nickname = raw.host_name.empty() ? raw.host_addr : raw.host_name;
            c.target = raw.target;
            c.local = PsLocalEndpoint{
                raw.host_addr, raw.host_request_port, raw.state, true};
            consoles.push_back(std::move(c));
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            lan_consoles_ = std::move(consoles);
        }
        mergeAndNotify(cb);
    });

    if (ok) discovering_.store(true);
    return ok;
}

void PsConsoleRepository::stopDiscovery() {
    if (!discovering_.exchange(false)) return;
    if (discovery_) discovery_->stop();
}

bool PsConsoleRepository::fetchPsnDevices(const std::string& access_token,
                                           HostListCallback cb, std::string* error) {
    constexpr const char* url =
        "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/clients"
        "?platform=PS5&includeFields=device&limit=10&offset=0";
    api::HttpClient http;
    auto response = http.get(url, {
        {"Authorization", "Bearer " + access_token},
        {"Accept", "application/json"},
        {"User-Agent", "remoteplay Windows"},
    });
    last_psn_status_code_.store(response.status_code);
    if (response.network_error || response.status_code != 200) {
        if (error) {
            *error = response.network_error
                ? "PSN console list request timed out or failed: " + response.error_message
                : "PSN console list returned HTTP " + std::to_string(response.status_code);
        }
        diagnosticLog("ps-console-repository", "PSN device list failed status=%d network=%s",
                      response.status_code, response.network_error ? "true" : "false");
        return false;
    }

    cJSON* root = cJSON_Parse(response.body.c_str());
    cJSON* clients = root ? cJSON_GetObjectItemCaseSensitive(root, "clients") : nullptr;
    if (!cJSON_IsArray(clients)) {
        if (root) cJSON_Delete(root);
        if (error) *error = "PSN console list response is invalid";
        return false;
    }

    std::vector<PsConsole> fetched;
    cJSON* client = nullptr;
    cJSON_ArrayForEach(client, clients) {
        cJSON* duid = cJSON_GetObjectItemCaseSensitive(client, "duid");
        cJSON* device = cJSON_GetObjectItemCaseSensitive(client, "device");
        cJSON* name = device ? cJSON_GetObjectItemCaseSensitive(device, "name") : nullptr;
        cJSON* features = device ? cJSON_GetObjectItemCaseSensitive(device, "enabledFeatures") : nullptr;
        if (!cJSON_IsString(duid) || !cJSON_IsObject(device) ||
            !cJSON_IsString(name) || !cJSON_IsArray(features)) continue;

        std::string uid;
        if (!cJSON_IsString(duid) || !normalizeDuid(duid->valuestring)) continue;
        uid = *normalizeDuid(duid->valuestring);
        bool remoteplay_enabled = false;
        cJSON* feature = nullptr;
        cJSON_ArrayForEach(feature, features) {
            if (cJSON_IsString(feature) && std::strcmp(feature->valuestring, "remotePlay") == 0) {
                remoteplay_enabled = true;
                break;
            }
        }

        PsConsole console;
        console.stable_id = "duid:" + uid;
        console.psn_duid = uid;
        console.nickname = name->valuestring;
        console.target = CHIAKI_TARGET_PS5_1;
        console.remote = PsRemoteEndpoint{uid, name->valuestring, remoteplay_enabled};
        fetched.push_back(std::move(console));
    }
    cJSON_Delete(root);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        psn_consoles_ = std::move(fetched);
    }
    diagnosticLog("ps-console-repository", "PSN device list complete count=%zu",
                  psn_consoles_.size());
    mergeAndNotify(std::move(cb));
    return true;
}

std::vector<PsConsole> PsConsoleRepository::getUnifiedList() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PsConsole> unified = lan_consoles_;

    for (const auto& credential : credentials_) {
        auto it = std::find_if(unified.begin(), unified.end(),
            [&](const PsConsole& console) {
                return !console.server_mac.empty() &&
                       console.server_mac == credential.server_mac;
            });
        if (it == unified.end()) {
            PsConsole console;
            console.server_mac = credential.server_mac;
            console.psn_duid = credential.psn_duid;
            console.stable_id = "mac:" + credential.server_mac;
            console.nickname = credential.nickname;
            console.target = credential.target;
            console.credentials = credential;
            // Keep the historical address visible, but do not claim it is an
            // online route until discovery (or pairing in this process)
            // verifies it.
            if (!credential.last_known_addr.empty()) {
                console.local = PsLocalEndpoint{
                    credential.last_known_addr, 0, PsConsoleState::Unknown, false};
            }
            unified.push_back(std::move(console));
        } else {
            it->credentials = credential;
            it->target = credential.target;
            if (it->nickname.empty()) it->nickname = credential.nickname;
            if (it->psn_duid.empty()) it->psn_duid = credential.psn_duid;
            if (!it->local.has_value() && !credential.last_known_addr.empty()) {
                it->local = PsLocalEndpoint{
                    credential.last_known_addr, 0, PsConsoleState::Unknown, false};
            }
        }
    }

    for (const auto& psn : psn_consoles_) {
        auto it = std::find_if(unified.begin(), unified.end(),
            [&](const PsConsole& console) {
                return !console.psn_duid.empty() &&
                       console.psn_duid == psn.psn_duid;
            });
        if (it != unified.end()) {
            it->remote = psn.remote;
            // The PSN device list is authoritative for remote-play identity.
            // A LAN/credential entry may have been paired as PS4 previously;
            // retaining that target makes the later session send /sie/ps4/*
            // even though the PSN DUID belongs to a PS5.
            it->target = psn.target;
            if (it->psn_duid.empty()) it->psn_duid = psn.psn_duid;
        } else {
            unified.push_back(psn);
        }
    }
    return unified;
}

void PsConsoleRepository::setRegisteredCredentials(
    std::vector<RegisteredCredential> credentials) {
    std::lock_guard<std::mutex> lock(mutex_);
    credentials_ = std::move(credentials);
}

void PsConsoleRepository::notePairedLocalHost(
    const RegisteredCredential& credential) {
    if (credential.last_known_addr.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(lan_consoles_.begin(), lan_consoles_.end(),
        [&](const PsConsole& console) {
            return !console.server_mac.empty() &&
                   console.server_mac == credential.server_mac;
        });
    if (it == lan_consoles_.end()) {
        PsConsole console;
        console.stable_id = "mac:" + credential.server_mac;
        console.server_mac = credential.server_mac;
        console.nickname = credential.nickname.empty()
            ? credential.last_known_addr : credential.nickname;
        console.target = credential.target;
        console.credentials = credential;
        console.local = PsLocalEndpoint{
            credential.last_known_addr, 0, PsConsoleState::Ready, true};
        lan_consoles_.push_back(std::move(console));
        return;
    }

    it->target = credential.target;
    it->credentials = credential;
    it->local = PsLocalEndpoint{
        credential.last_known_addr, 0, PsConsoleState::Ready, true};
}

void PsConsoleRepository::mergeAndNotify(HostListCallback cb) {
    if (!cb) return;
    auto hosts = getUnifiedList();
    brls::sync([cb = std::move(cb), hosts = std::move(hosts)]() {
        cb(hosts);
    });
}

} // namespace lunar::ps

#endif
