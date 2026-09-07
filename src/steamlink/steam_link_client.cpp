#ifdef __SWITCH__

#include "steam_link_client.h"

#include "../common.h"
#include "../diagnostics.h"

#include <cJSON.h>
#include <switch.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" char* IHS_IPAddressToString(const IHS_IPAddress* address) {
    if (!address || address->family != IHS_IPAddressFamilyIPv4) return nullptr;
    char* result = static_cast<char*>(std::malloc(16));
    if (!result) return nullptr;
    std::snprintf(result, 16, "%u.%u.%u.%u",
                  address->v4.data[0], address->v4.data[1],
                  address->v4.data[2], address->v4.data[3]);
    return result;
}

namespace lunar::steamlink {
namespace {

constexpr const char* kDeviceIdKey = "device_id";
constexpr const char* kSecretKey = "secret_key";
constexpr const char* kDeviceName = "LunarNX";

bool readHex(const char* text, uint8_t* output, size_t output_size) {
    if (!text || !output || std::strlen(text) != output_size * 2) return false;
    for (size_t i = 0; i < output_size; ++i) {
        unsigned value = 0;
        if (std::sscanf(text + i * 2, "%2x", &value) != 1 || value > 0xff) {
            return false;
        }
        output[i] = static_cast<uint8_t>(value);
    }
    return true;
}

std::string writeHex(const uint8_t* bytes, size_t size) {
    std::string result(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        std::snprintf(result.data() + i * 2, 3, "%02x", bytes[i]);
    }
    return result;
}

bool saveIdentity(const SteamLinkClient::Identity& identity) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, kDeviceIdKey,
                            writeHex(reinterpret_cast<const uint8_t*>(&identity.device_id),
                                     sizeof(identity.device_id)).c_str());
    cJSON_AddStringToObject(root, kSecretKey,
                            writeHex(identity.secret_key.data(), identity.secret_key.size()).c_str());
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;

    std::string temporary_path = std::string(lunar::get_steam_link_device_path()) + ".tmp";
    FILE* output = std::fopen(temporary_path.c_str(), "wb");
    bool written = false;
    if (output) {
        written = std::fwrite(json, 1, std::strlen(json), output) == std::strlen(json);
        written = std::fclose(output) == 0 && written;
    }
    std::free(json);
    if (!written) {
        std::remove(temporary_path.c_str());
        return false;
    }
    return std::rename(temporary_path.c_str(), lunar::get_steam_link_device_path()) == 0;
}

bool loadIdentity(SteamLinkClient::Identity* identity) {
    FILE* input = std::fopen(lunar::get_steam_link_device_path(), "rb");
    if (!input) return false;
    std::fseek(input, 0, SEEK_END);
    const long length = std::ftell(input);
    std::rewind(input);
    if (length <= 0 || length > 4096) {
        std::fclose(input);
        return false;
    }
    std::string data(static_cast<size_t>(length), '\0');
    const bool read = std::fread(data.data(), 1, data.size(), input) == data.size();
    std::fclose(input);
    if (!read) return false;

    cJSON* root = cJSON_Parse(data.c_str());
    if (!root) return false;
    const cJSON* device_id = cJSON_GetObjectItemCaseSensitive(root, kDeviceIdKey);
    const cJSON* secret_key = cJSON_GetObjectItemCaseSensitive(root, kSecretKey);
    bool valid = device_id && cJSON_IsString(device_id) && secret_key &&
                 cJSON_IsString(secret_key);
    uint8_t device_bytes[sizeof(identity->device_id)]{};
    if (valid) valid = readHex(device_id->valuestring, device_bytes, sizeof(device_bytes));
    if (valid) valid = readHex(secret_key->valuestring, identity->secret_key.data(),
                               identity->secret_key.size());
    if (valid) std::memcpy(&identity->device_id, device_bytes, sizeof(device_bytes));
    cJSON_Delete(root);
    return valid && identity->device_id != 0;
}

} // namespace

SteamLinkClient::SteamLinkClient() {
    if (!loadOrCreateIdentity()) {
        last_error_ = "Could not create Steam Link device identity";
    }
}

SteamLinkClient::~SteamLinkClient() {
    stopDiscovery();
    cancelAuthorization();
    if (client_) {
        IHS_ClientStop(client_);
        IHS_ClientThreadedJoin(client_);
        IHS_ClientDestroy(client_);
        client_ = nullptr;
    }
    if (ihs_initialized_) {
        IHS_Quit();
        ihs_initialized_ = false;
    }
}

bool SteamLinkClient::loadOrCreateIdentity() {
    if (loadIdentity(&identity_)) return true;
    randomGet(&identity_.device_id, sizeof(identity_.device_id));
    randomGet(identity_.secret_key.data(), identity_.secret_key.size());
    if (identity_.device_id == 0) identity_.device_id = 1;
    ensureDiagnosticLogDirectory();
    return saveIdentity(identity_);
}

bool SteamLinkClient::ensureClient() {
    if (client_) return true;
    if (identity_.device_id == 0) {
        last_error_ = "Steam Link device identity is unavailable";
        return false;
    }
    IHS_Init();
    ihs_initialized_ = true;
    IHS_ClientConfig config{};
    config.deviceId = identity_.device_id;
    config.secretKey = identity_.secret_key.data();
    config.deviceName = kDeviceName;
    client_ = IHS_ClientCreate(&config);
    if (!client_) {
        last_error_ = "IHS client allocation failed";
        IHS_Quit();
        ihs_initialized_ = false;
        return false;
    }
    static const IHS_ClientDiscoveryCallbacks discovery_callbacks{
        &SteamLinkClient::onDiscovered};
    static const IHS_ClientAuthorizationCallbacks authorization_callbacks{
        &SteamLinkClient::onAuthorizationProgress,
        &SteamLinkClient::onAuthorizationSuccess,
        &SteamLinkClient::onAuthorizationFailed};
    IHS_ClientSetDiscoveryCallbacks(client_, &discovery_callbacks, this);
    IHS_ClientSetAuthorizationCallbacks(client_, &authorization_callbacks, this);
    return true;
}

bool SteamLinkClient::startDiscovery(HostCallback callback) {
    if (!ensureClient()) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        host_callback_ = std::move(callback);
        hosts_.clear();
        host_infos_.clear();
    }
    if (!IHS_ClientStartDiscovery(client_, 2500)) {
        last_error_ = "Steam Link discovery is already running";
        return false;
    }
    return true;
}

void SteamLinkClient::stopDiscovery() {
    if (client_) IHS_ClientStopDiscovery(client_);
    std::lock_guard<std::mutex> lock(mutex_);
    host_callback_ = {};
}

bool SteamLinkClient::findHost(uint64_t client_id, IHS_HostInfo* result) const {
    if (!result) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = host_infos_.find(client_id);
    if (it == host_infos_.end()) return false;
    *result = it->second;
    return true;
}

bool SteamLinkClient::authorize(const SteamLinkHost& host, const std::string& pin,
                                AuthorizationCallback callback) {
    if (pin.size() != 4 || !std::all_of(pin.begin(), pin.end(),
                                        [](char c) { return c >= '0' && c <= '9'; })) {
        last_error_ = "Steam Link pairing code must contain four digits";
        return false;
    }
    if (!ensureClient()) return false;
    IHS_HostInfo protocol_host{};
    if (!findHost(host.client_id, &protocol_host)) {
        last_error_ = "Steam Link host is no longer in the discovery list";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        authorization_callback_ = std::move(callback);
    }
    if (!IHS_ClientAuthorizationRequest(client_, &protocol_host, pin.c_str())) {
        std::lock_guard<std::mutex> lock(mutex_);
        authorization_callback_ = {};
        last_error_ = "Steam Link authorization is already in progress";
        return false;
    }
    return true;
}

void SteamLinkClient::cancelAuthorization() {
    if (client_) IHS_ClientAuthorizationCancel(client_);
    std::lock_guard<std::mutex> lock(mutex_);
    authorization_callback_ = {};
}

bool SteamLinkClient::isAuthorized(uint64_t client_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return authorized_client_id_ == client_id;
}

void SteamLinkClient::updateHost(const IHS_HostInfo& host) {
    HostCallback callback;
    std::vector<SteamLinkHost> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        host_infos_[host.clientId] = host;
        auto it = std::find_if(hosts_.begin(), hosts_.end(),
            [&host](const SteamLinkHost& item) { return item.client_id == host.clientId; });
        SteamLinkHost item;
        item.client_id = host.clientId;
        item.instance_id = host.instanceId;
        item.hostname = host.hostname;
        char* address = IHS_IPAddressToString(&host.address.ip);
        if (address) {
            item.address = address;
            std::free(address);
        }
        item.port = host.address.port;
        item.ostype = static_cast<int>(host.ostype);
        item.games_running = host.gamesRunning;
        if (it == hosts_.end()) hosts_.push_back(std::move(item));
        else *it = std::move(item);
        callback = host_callback_;
        snapshot = hosts_;
    }
    if (callback) callback(snapshot);
}

void SteamLinkClient::emitHosts() {
    HostCallback callback;
    std::vector<SteamLinkHost> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = host_callback_;
        snapshot = hosts_;
    }
    if (callback) callback(snapshot);
}

void SteamLinkClient::onDiscovered(IHS_Client*, const IHS_HostInfo* host, void* context) {
    if (!context || !host) return;
    static_cast<SteamLinkClient*>(context)->updateHost(*host);
}

void SteamLinkClient::onAuthorizationProgress(IHS_Client*, const IHS_HostInfo*, void* context) {
    auto* self = static_cast<SteamLinkClient*>(context);
    if (!self) return;
    self->emitHosts();
}

void SteamLinkClient::onAuthorizationSuccess(IHS_Client*, const IHS_HostInfo* host,
                                             uint64_t, void* context) {
    auto* self = static_cast<SteamLinkClient*>(context);
    if (!self || !host) return;
    AuthorizationCallback callback;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->authorized_client_id_ = host->clientId;
        callback = self->authorization_callback_;
        self->authorization_callback_ = {};
    }
    if (callback) callback(true, {});
}

std::string SteamLinkClient::authorizationError(IHS_AuthorizationResult result) {
    switch (result) {
        case IHS_AuthorizationDenied: return "Steam rejected the PIN";
        case IHS_AuthorizationNotLoggedIn: return "Steam is not signed in on the host";
        case IHS_AuthorizationOffline: return "Steam host is offline";
        case IHS_AuthorizationBusy: return "Steam host is busy";
        case IHS_AuthorizationTimedOut: return "Steam PIN request timed out";
        case IHS_AuthorizationCanceled: return "Steam PIN request canceled";
        default: return "Steam Link authorization failed";
    }
}

void SteamLinkClient::onAuthorizationFailed(IHS_Client*, const IHS_HostInfo*,
                                            IHS_AuthorizationResult result, void* context) {
    auto* self = static_cast<SteamLinkClient*>(context);
    if (!self) return;
    AuthorizationCallback callback;
    const std::string error = authorizationError(result);
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        callback = self->authorization_callback_;
        self->authorization_callback_ = {};
    }
    if (callback) callback(false, error);
}

} // namespace lunar::steamlink
#endif
