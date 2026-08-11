#ifdef __SWITCH__

#include "psn_auth_manager.h"
#include "psn_auth_utils.h"
#include "../api/http_client.h"
#include "../diagnostics.h"

#include <switch.h>
#include <cJSON.h>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

constexpr char kClientId[] = "ba495a24-818c-472b-b12d-ff231c1b5745";
constexpr char kClientSecret[] = "mvaiZkRsAsI1IBkY";
constexpr char kRedirectUri[] =
    "https://remoteplay.dl.playstation.net/remoteplay/redirect";
constexpr char kAuthorizeUrl[] =
    "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize";
constexpr char kTokenUrl[] =
    "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token";
constexpr char kScope[] =
    "psn:clientapp referenceDataService:countryConfig.read "
    "pushNotification:webSocket.desktop.connect "
    "sessionManager:remotePlaySession.system.update";
constexpr size_t kDuidLength = CHIAKI_DUID_STR_SIZE - 1;
constexpr uint64_t kRemoteSessionTokenMarginMs = 10 * 60 * 1000;

uint64_t nowMilliseconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string urlEncode(const std::string& input) {
    std::string output;
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            output.push_back(static_cast<char>(c));
        } else {
            char encoded[4];
            std::snprintf(encoded, sizeof(encoded), "%%%02X", c);
            output += encoded;
        }
    }
    return output;
}

std::string formEncode(const std::map<std::string, std::string>& params) {
    std::string body;
    for (const auto& [key, value] : params) {
        if (!body.empty()) body.push_back('&');
        body += urlEncode(key) + "=" + urlEncode(value);
    }
    return body;
}

std::string jsonString(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return item && cJSON_IsString(item) ? item->valuestring : std::string{};
}

int jsonInt(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return item && cJSON_IsNumber(item) ? item->valueint : 0;
}

std::string basicAuthorization() {
    std::string credentials = std::string(kClientId) + ":" + kClientSecret;
    return "Basic " + lunar::ps::base64Encode(
        reinterpret_cast<const uint8_t*>(credentials.data()), credentials.size());
}

} // namespace

namespace lunar::ps {

PsnAuthManager::PsnAuthManager() = default;
PsnAuthManager::~PsnAuthManager() = default;

bool PsnAuthManager::fail(const std::string& message, StateCallback cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = message;
    }
    state_.store(PsnAuthState::Error);
    diagnosticLog("psn-auth", "%s", message.c_str());
    if (cb) cb(PsnAuthState::Error, message);
    return false;
}

std::string PsnAuthManager::startAuth() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!login_url_.empty()) return login_url_;

    if (duid_.size() != kDuidLength) {
        char duid_buffer[CHIAKI_DUID_STR_SIZE]{};
        size_t buffer_size = sizeof(duid_buffer);
        ChiakiErrorCode err =
            chiaki_holepunch_generate_client_device_uid(duid_buffer, &buffer_size);

        // chiaki-ng currently mutates buffer_size as an accumulator. The buffer
        // itself is NUL-terminated, so never use the returned size as its length.
        size_t generated_length = strnlen(duid_buffer, sizeof(duid_buffer));
        if (err != CHIAKI_ERR_SUCCESS || generated_length != kDuidLength) {
            error_ = "Failed to generate a valid PSN device ID";
            state_.store(PsnAuthState::Error);
            diagnosticLog("psn-auth", "DUID generation failed: err=%s length=%zu",
                          chiaki_error_string(err), generated_length);
            return {};
        }
        duid_.assign(duid_buffer, generated_length);
    }

    login_url_ =
        std::string(kAuthorizeUrl) +
        "?service_entity=urn:service-entity:psn"
        "&response_type=code"
        "&client_id=" + urlEncode(kClientId) +
        "&redirect_uri=" + urlEncode(kRedirectUri) +
        "&scope=" + urlEncode(kScope) +
        "&request_locale=en_US"
        "&ui=pr"
        "&service_logo=ps"
        "&layout_type=popup"
        "&smcid=remoteplay"
        "&prompt=always"
        "&PlatformPrivacyWs1=minimal"
        "&duid=" + urlEncode(duid_);

    error_.clear();
    state_.store(PsnAuthState::WaitingForCode);
    diagnosticLog("psn-auth", "Auth URL ready");
    return login_url_;
}

bool PsnAuthManager::openWebApplet(std::string& authorization_code) {
    authorization_code.clear();
    std::string login_url = getLoginUrl();
    if (login_url.empty()) return fail("Call startAuth() first");

    WebCommonConfig config{};
    Result rc = webPageCreate(&config, login_url.c_str());
    if (R_FAILED(rc)) {
        return fail("Unable to open the Switch browser (webPageCreate " +
                    std::to_string(rc) + ")");
    }

    rc = webConfigSetCallbackUrl(&config, kRedirectUri);
    if (R_FAILED(rc)) {
        return fail("Switch browser callback URL is unavailable (" +
                    std::to_string(rc) + ")");
    }

    WebCommonReply reply{};
    rc = webConfigShow(&config, &reply);
    if (R_FAILED(rc)) {
        return fail("Switch browser failed (webConfigShow " + std::to_string(rc) + ")");
    }

    WebExitReason reason = WebExitReason_UnknownE;
    rc = webReplyGetExitReason(&reply, &reason);
    if (R_FAILED(rc) || reason != WebExitReason_LastUrl) {
        return fail(reason == WebExitReason_ExitButton || reason == WebExitReason_BackButton
                        ? "Login cancelled"
                        : "Switch browser closed before Sony returned a login code");
    }

    char last_url[0x1000]{};
    size_t url_size = 0;
    rc = webReplyGetLastUrl(&reply, last_url, sizeof(last_url), &url_size);
    if (R_FAILED(rc) || url_size <= 1 || url_size > sizeof(last_url)) {
        return fail("Failed to read Sony's redirect URL");
    }

    authorization_code = extractPsnAuthorizationCode(
        std::string(last_url, strnlen(last_url, sizeof(last_url))));
    if (authorization_code.empty()) return fail("Sony redirect did not contain a login code");

    diagnosticLog("psn-auth", "Switch browser captured Sony callback code");
    return true;
}

bool PsnAuthManager::submitRedirectUrl(const std::string& input, StateCallback cb) {
    std::string code = extractPsnAuthorizationCode(input);
    if (code.empty()) return fail("Enter the full Sony redirect URL or authorization code", cb);
    return exchangeCodeForToken(code, std::move(cb));
}

bool PsnAuthManager::requestToken(const std::map<std::string, std::string>& params,
                                  bool preserve_refresh_token) {
    api::HttpClient http;
    std::map<std::string, std::string> headers{
        {"Authorization", basicAuthorization()},
        {"Content-Type", "application/x-www-form-urlencoded"},
    };

    auto response = http.post(kTokenUrl, formEncode(params), headers);
    if (response.status_code < 200 || response.status_code >= 300) {
        std::string detail = response.network_error ? response.error_message
            : "HTTP " + std::to_string(response.status_code);
        return fail("PSN token request failed: " + detail);
    }

    cJSON* root = cJSON_Parse(response.body.c_str());
    if (!root) return fail("PSN token request returned invalid JSON");

    std::string access_token = jsonString(root, "access_token");
    std::string refresh_token = jsonString(root, "refresh_token");
    int expires_in = jsonInt(root, "expires_in");
    cJSON_Delete(root);

    if (access_token.empty()) return fail("PSN token response has no access_token");
    if (expires_in <= 0) expires_in = 3600;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        access_token_ = std::move(access_token);
        if (!refresh_token.empty() || !preserve_refresh_token) {
            refresh_token_ = std::move(refresh_token);
        }
        expires_in_ = expires_in;
        expires_at_ms_ = nowMilliseconds() + static_cast<uint64_t>(expires_in) * 1000ULL;
        error_.clear();
    }
    return true;
}

bool PsnAuthManager::exchangeCodeForToken(const std::string& code, StateCallback cb) {
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    state_.store(PsnAuthState::ExchangingCode);
    std::map<std::string, std::string> params{
        {"grant_type", "authorization_code"},
        {"code", code},
        {"redirect_uri", kRedirectUri},
        {"scope", kScope},
    };
    if (!requestToken(params, false)) {
        if (cb) cb(PsnAuthState::Error, getAuthError());
        return false;
    }
    if (!fetchAccountId()) {
        return fail("Signed in, but failed to retrieve the PSN account ID", cb);
    }

    state_.store(PsnAuthState::Authenticated);
    diagnosticLog("psn-auth", "Authenticated with PSN account ID");
    if (cb) cb(PsnAuthState::Authenticated, {});
    return true;
}

bool PsnAuthManager::refreshAccessToken() {
    std::string refresh_token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        refresh_token = refresh_token_;
    }
    if (refresh_token.empty()) return fail("PSN session has no refresh token");

    state_.store(PsnAuthState::ExchangingCode);
    std::map<std::string, std::string> params{
        {"grant_type", "refresh_token"},
        {"refresh_token", refresh_token},
        {"redirect_uri", kRedirectUri},
        {"scope", kScope},
    };
    if (!requestToken(params, true)) return false;

    if ((getAccountId().empty() || getOnlineId().empty()) && !fetchAccountId()) {
        return fail("PSN token refreshed, but account ID lookup failed");
    }
    state_.store(PsnAuthState::Authenticated);
    diagnosticLog("psn-auth", "PSN access token refreshed");
    return true;
}

bool PsnAuthManager::refreshIdentity() {
    return fetchAccountId();
}

bool PsnAuthManager::ensureValidToken(StateCallback cb) {
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    if (hasValidToken()) {
        if (cb) cb(PsnAuthState::Authenticated, {});
        return true;
    }
    if (!refreshAccessToken()) {
        if (cb) cb(PsnAuthState::Error, getAuthError());
        return false;
    }
    if (cb) cb(PsnAuthState::Authenticated, {});
    return true;
}

bool PsnAuthManager::refreshToken(StateCallback cb) {
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    if (!refreshAccessToken()) {
        if (cb) cb(PsnAuthState::Error, getAuthError());
        return false;
    }
    if (cb) cb(PsnAuthState::Authenticated, {});
    return true;
}

bool PsnAuthManager::fetchAccountId() {
    std::string token = getAccessToken();
    if (token.empty()) return false;

    api::HttpClient http;
    std::map<std::string, std::string> headers{{"Authorization", basicAuthorization()}};
    auto response = http.get(std::string(kTokenUrl) + "/" + urlEncode(token), headers);
    if (response.status_code < 200 || response.status_code >= 300) return false;

    cJSON* root = cJSON_Parse(response.body.c_str());
    if (!root) return false;
    std::string user_id = jsonString(root, "user_id");
    std::string online_id = jsonString(root, "online_id");
    cJSON_Delete(root);
    if (user_id.empty()) return false;

    try {
        size_t consumed = 0;
        unsigned long long uid = std::stoull(user_id, &consumed, 10);
        if (consumed != user_id.size()) return false;
        uint8_t bytes[8]{};
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = static_cast<uint8_t>((uid >> (i * 8)) & 0xff);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        account_id_ = base64Encode(bytes, sizeof(bytes));
        online_id_ = online_id;
        return true;
    } catch (...) {
        return false;
    }
}

bool PsnAuthManager::hasValidToken() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !access_token_.empty() &&
        (expires_at_ms_ == 0 ||
         expires_at_ms_ > nowMilliseconds() + kRemoteSessionTokenMarginMs);
}

bool PsnAuthManager::hasStoredSession() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !access_token_.empty() || !refresh_token_.empty();
}

bool PsnAuthManager::loadToken(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (length <= 0 || length > 65536) {
        std::fclose(file);
        return false;
    }

    std::string content(static_cast<size_t>(length), '\0');
    size_t read = std::fread(content.data(), 1, content.size(), file);
    std::fclose(file);
    if (read != content.size()) return false;

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) return false;
    std::string access_token = jsonString(root, "access_token");
    std::string refresh_token = jsonString(root, "refresh_token");
    std::string account_id = jsonString(root, "account_id");
    std::string online_id = jsonString(root, "online_id");
    std::string duid = jsonString(root, "duid");
    cJSON* expiry = cJSON_GetObjectItem(root, "expires_at_ms");
    cJSON* lifetime = cJSON_GetObjectItem(root, "expires_in");
    uint64_t expires_at = expiry && cJSON_IsNumber(expiry)
        ? static_cast<uint64_t>(expiry->valuedouble) : 0;
    int expires_in = lifetime && cJSON_IsNumber(lifetime) ? lifetime->valueint : 3600;
    cJSON_Delete(root);

    if (access_token.empty() && refresh_token.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        access_token_ = std::move(access_token);
        refresh_token_ = std::move(refresh_token);
        account_id_ = std::move(account_id);
        online_id_ = std::move(online_id);
        if (duid.size() == kDuidLength) duid_ = std::move(duid);
        expires_in_ = expires_in;
        expires_at_ms_ = expires_at;
    }
    state_.store(hasValidToken() ? PsnAuthState::Authenticated : PsnAuthState::Idle);
    return true;
}

bool PsnAuthManager::saveToken(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "access_token", access_token_.c_str());
    cJSON_AddStringToObject(root, "refresh_token", refresh_token_.c_str());
    cJSON_AddStringToObject(root, "account_id", account_id_.c_str());
    cJSON_AddStringToObject(root, "online_id", online_id_.c_str());
    cJSON_AddStringToObject(root, "duid", duid_.c_str());
    cJSON_AddNumberToObject(root, "expires_in", expires_in_);
    cJSON_AddNumberToObject(root, "expires_at_ms", static_cast<double>(expires_at_ms_));

    char* json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return false;
    FILE* file = std::fopen(path.c_str(), "w");
    if (!file) {
        std::free(json);
        return false;
    }
    bool ok = std::fputs(json, file) >= 0;
    ok = std::fclose(file) == 0 && ok;
    std::free(json);
    return ok;
}

void PsnAuthManager::signOut() {
    std::lock_guard<std::mutex> lock(mutex_);
    access_token_.clear();
    refresh_token_.clear();
    account_id_.clear();
    online_id_.clear();
    login_url_.clear();
    error_.clear();
    expires_in_ = 3600;
    expires_at_ms_ = 0;
    state_.store(PsnAuthState::Idle);
}

std::string PsnAuthManager::getAccessToken() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return access_token_;
}

std::string PsnAuthManager::getAccountId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return account_id_;
}

std::string PsnAuthManager::getOnlineId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return online_id_;
}

std::string PsnAuthManager::getDuid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return duid_;
}

std::string PsnAuthManager::getAuthError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

std::string PsnAuthManager::getLoginUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return login_url_;
}

} // namespace lunar::ps

#endif
