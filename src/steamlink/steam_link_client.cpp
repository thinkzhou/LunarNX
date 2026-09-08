#ifdef __SWITCH__

#include "steam_link_client.h"
#include "stream_support.h"
extern "C" {
#include "ihs_streaming_support.h"
}

#include "../common.h"
#include "../diagnostics.h"

#include <cJSON.h>
#include <switch.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    lunar::diagnosticLog("steam-link", "client construct");
    if (!loadOrCreateIdentity()) {
        last_error_ = "Could not create Steam Link device identity";
        lunar::persistentEventLog("steam-link", "identity unavailable");
    } else {
        lunar::diagnosticLog("steam-link", "identity ready device_id=%llu",
                             static_cast<unsigned long long>(identity_.device_id));
    }
}

SteamLinkClient::~SteamLinkClient() {
    lunar::diagnosticLog("steam-link", "client destruct");
    stopDiscovery();
    cancelAuthorization();
    cancelStreaming();
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
    static const IHS_ClientStreamingCallbacks streaming_callbacks{
        &SteamLinkClient::onStreamingProgress,
        &SteamLinkClient::onStreamingSuccess,
        &SteamLinkClient::onStreamingFailed};
    IHS_ClientSetDiscoveryCallbacks(client_, &discovery_callbacks, this);
    IHS_ClientSetAuthorizationCallbacks(client_, &authorization_callbacks, this);
    IHS_ClientSetStreamingCallbacks(client_, &streaming_callbacks, this);
    IHS_ClientSetLogFunction(client_, &SteamLinkClient::logFunction);
    lunar::diagnosticLog("steam-link", "ihslib client ready");
    return true;
}

bool SteamLinkClient::startDiscovery(HostCallback callback) {
    if (!ensureClient()) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        host_callback_ = std::move(callback);
    }
    // Refresh reuses the periodic discovery timer and preserves hosts needed by
    // pairing. Restarting ihslib's asynchronous timer can race its end callback.
    if (discovery_started_) {
        emitHosts();
        return true;
    }
    if (!IHS_ClientStartDiscovery(client_, 2500)) {
        last_error_ = "Steam Link discovery is already running";
        return false;
    }
    discovery_started_ = true;
    emitHosts();
    lunar::diagnosticLog("steam-link", "discovery started interval_ms=2500");
    return true;
}

void SteamLinkClient::stopDiscovery() {
    if (client_) IHS_ClientStopDiscovery(client_);
    discovery_started_ = false;
    lunar::diagnosticLog("steam-link", "discovery stopped");
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
    lunar::diagnosticLog("steam-link", "authorization requested host=%s pin_len=%zu",
                         host.address.c_str(), pin.size());
    return true;
}

bool SteamLinkClient::requestStreaming(const SteamLinkHost& host, const std::string& pin,
                                       int width, int height, StreamingCallback callback) {
    std::lock_guard<std::mutex> operation(streaming_operation_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (streaming_callback_) {
            last_error_ = "Steam Link streaming request is already in progress";
            return false; // Never replace the active request's callback.
        }
    }
    if (pin.size() >= 16 || !std::all_of(pin.begin(), pin.end(), [](char c) {
            return c >= '0' && c <= '9';
        })) {
        last_error_ = "Steam host security PIN must contain digits only";
        return false;
    }
    if (!ensureClient()) return false;
    // A completed callback may precede the timer's deferred cleanup. Drain it
    // before installing the next callback; late packets are matched by ID.
    LunarIHSStreamingCancel(client_);
    IHS_HostInfo protocol_host{};
    if (!findHost(host.client_id, &protocol_host)) {
        last_error_ = "Steam Link host is no longer in the discovery list";
        return false;
    }
    IHS_StreamingRequest request{};
    std::snprintf(request.pin, sizeof(request.pin), "%s", pin.c_str());
    request.streamingEnable.video = true;
    request.streamingEnable.audio = true;
    request.streamingEnable.input = true;
    request.maxResolution.x = width > 0 ? width : 1280;
    request.maxResolution.y = height > 0 ? height : 720;
    request.audioChannelCount = 2;
    request.streamingInterface = IHS_StreamInterfaceBigPicture;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        streaming_callback_ = std::move(callback);
    }
    if (!IHS_ClientStreamingRequest(client_, &protocol_host, &request)) {
        std::lock_guard<std::mutex> lock(mutex_);
        streaming_callback_ = {};
        last_error_ = "Steam Link streaming request is already in progress";
        return false;
    }
    lunar::diagnosticLog("steam-link", "streaming requested host=%s pin_len=%zu profile=%dx%d",
                         host.address.c_str(), pin.size(), request.maxResolution.x,
                         request.maxResolution.y);
    return true;
}

void SteamLinkClient::cancelStreaming() {
    std::lock_guard<std::mutex> operation(streaming_operation_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        streaming_callback_ = {};
    }
    // Do not hold mutex_: an executing protocol callback may need it.
    if (client_) LunarIHSStreamingCancel(client_);
    lunar::diagnosticLog("steam-link", "streaming request drained");
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

uint64_t SteamLinkClient::authorizedSteamId(uint64_t client_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = authorized_steam_ids_.find(client_id);
    return it == authorized_steam_ids_.end() ? 0 : it->second;
}

bool SteamLinkClient::getSessionClientConfig(IHS_ClientConfig* config) const {
    if (!config || identity_.device_id == 0) return false;
    config->deviceId = identity_.device_id;
    config->secretKey = identity_.secret_key.data();
    config->deviceName = kDeviceName;
    return true;
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
                                             uint64_t steam_id, void* context) {
    auto* self = static_cast<SteamLinkClient*>(context);
    if (!self || !host) return;
    AuthorizationCallback callback;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->authorized_client_id_ = host->clientId;
        self->authorized_steam_ids_[host->clientId] = steam_id;
        callback = self->authorization_callback_;
        self->authorization_callback_ = {};
    }
    if (callback) callback(true, {});
}

void SteamLinkClient::onStreamingProgress(IHS_Client*, const IHS_HostInfo* host, void* context) {
    auto* self = static_cast<SteamLinkClient*>(context);
    if (!self || !host) return;
    lunar::diagnosticLog("steam-link", "streaming progress host=%s", host->hostname);
}

void SteamLinkClient::onStreamingSuccess(IHS_Client*, const IHS_HostInfo* host,
                                         const IHS_SocketAddress* address,
                                         const uint8_t* session_key, size_t session_key_len,
                                         void* context) {
    auto* self = static_cast<SteamLinkClient*>(context);
    if (!self || !host || !address || !session_key ||
        session_key_len == 0 || session_key_len > 32) return;
    StreamingCallback callback;
    SteamLinkStreamInfo info;
    info.address = *address;
    std::copy(session_key, session_key + session_key_len, info.session_key.begin());
    info.session_key_len = session_key_len;
    info.steam_id = self->authorizedSteamId(host->clientId);
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        callback = self->streaming_callback_;
        self->streaming_callback_ = {};
    }
    lunar::diagnosticLog("steam-link", "streaming success host=%s port=%u key_len=%zu steam_id=%llu",
                         host->hostname, address->port, session_key_len,
                         static_cast<unsigned long long>(info.steam_id));
    if (callback) callback(true, info, {});
}

void SteamLinkClient::onStreamingFailed(IHS_Client*, const IHS_HostInfo* host,
                                        IHS_StreamingResult result, void* context) {
    auto* self = static_cast<SteamLinkClient*>(context);
    if (!self) return;
    StreamingCallback callback;
    const std::string error = streamingError(result);
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        callback = self->streaming_callback_;
        self->streaming_callback_ = {};
    }
    lunar::persistentEventLog("steam-link", "streaming failed host=%s result=%d error=%s",
                             host ? host->hostname : "unknown", static_cast<int>(result),
                             error.c_str());
    if (callback) callback(false, {}, error);
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

std::string SteamLinkClient::streamingError(IHS_StreamingResult result) {
    switch (result) {
        case IHS_StreamingUnauthorized: return "Steam rejected the device authorization";
        case IHS_StreamingScreenLocked: return "Steam host screen is locked";
        case IHS_StreamingBusy: return "Steam host is busy";
        case IHS_StreamingDisabled: return "Steam Remote Play is disabled";
        case IHS_StreamingPINRequired: return "Steam host security PIN is required or incorrect";
        case IHS_StreamingGameLaunchFailed: return "Steam could not launch the selected game";
        case IHS_StreamingTransportUnavailable: return "Steam streaming transport unavailable";
        case IHS_StreamingTimeout: return "Steam streaming request timed out";
        default: return "Steam Link streaming request failed";
    }
}

void SteamLinkClient::logFunction(IHS_LogLevel level, const char* tag, const char* message) {
    // Packet-level traces must never synchronously flush to the SD card.
    if (level >= IHS_LogLevelDebug) return;
    static LogThrottle warning_throttle;
    static LogThrottle info_throttle;
    const auto now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (level == IHS_LogLevelWarn && !warning_throttle.allow(now_ms)) return;
    const bool negotiation = tag && std::strcmp(tag, "SteamNegotiation") == 0;
    if (level == IHS_LogLevelInfo && !negotiation && !info_throttle.allow(now_ms)) return;
    const char* safe_tag = tag ? tag : "ihslib";
    const char* safe_message = message ? message : "";
    if (level <= IHS_LogLevelWarn || negotiation) {
        lunar::persistentEventLog("steam-ihs", "level=%s tag=%s message=%s",
                                  IHS_LogLevelName(level), safe_tag, safe_message);
    } else {
        lunar::diagnosticLog("steam-ihs", "level=%s tag=%s message=%s",
                             IHS_LogLevelName(level), safe_tag, safe_message);
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
