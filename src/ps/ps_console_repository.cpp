#ifdef __SWITCH__

#include "ps_console_repository.h"
#include "ps_discovery.h"
#include "../api/http_client.h"
#include "../diagnostics.h"
#include <borealis.hpp>
#include <cJSON.h>
#include <algorithm>
#include <cstring>

namespace lunar::ps {

PsConsoleRepository::PsConsoleRepository(ChiakiLog* log) : log_(log) {}

PsConsoleRepository::~PsConsoleRepository() {
    stopDiscovery();
}

bool PsConsoleRepository::startDiscovery(HostListCallback cb) {
    if (discovering_.load()) return true;

    discovery_ = std::make_unique<PsDiscovery>(log_);
    bool ok = discovery_->start([this, cb = std::move(cb)](
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
            c.target = raw.console_type == PsConsoleType::PS5 ? 1000100 : 900;
            c.local = PsLocalEndpoint{raw.host_addr, raw.host_request_port, raw.state};
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
            unified.push_back(std::move(console));
        } else {
            it->credentials = credential;
            if (it->nickname.empty()) it->nickname = credential.nickname;
            if (it->psn_duid.empty()) it->psn_duid = credential.psn_duid;
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

void PsConsoleRepository::mergeAndNotify(HostListCallback cb) {
    if (!cb) return;
    auto hosts = getUnifiedList();
    brls::sync([cb = std::move(cb), hosts = std::move(hosts)]() {
        cb(hosts);
    });
}

} // namespace lunar::ps

#endif
