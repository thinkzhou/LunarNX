#ifdef __SWITCH__

#include "psn_auth_manager.h"
#include "psn_auth_utils.h"
#include "../api/http_client.h"
#include "../diagnostics.h"

#include <switch.h>
#include <cJSON.h>
#include <chrono>
#include <cerrno>
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

bool PsnAuthManager::fail(const std::string& message, StateCallback cb,
                          PsnAuthErrorKind kind) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = message;
        error_kind_ = kind;
    }
    state_.store(PsnAuthState::Error);
    diagnosticLog("psn-auth", "%s", message.c_str());
    if (cb) cb(PsnAuthState::Error, message);
    return false;
}

std::string PsnAuthManager::startAuth() {
    // A new login supersedes token/identity work from an older login page.
    request_generation_.invalidate();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!login_url_.empty()) {
        error_.clear();
        error_kind_ = PsnAuthErrorKind::None;
        state_.store(PsnAuthState::WaitingForCode);
        return login_url_;
    }

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
            error_kind_ = PsnAuthErrorKind::Fatal;
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
    error_kind_ = PsnAuthErrorKind::None;
    state_.store(PsnAuthState::WaitingForCode);
    diagnosticLog("psn-auth", "Auth URL ready");
    return login_url_;
}

bool PsnAuthManager::openWebApplet(std::string& authorization_code) {
    authorization_code.clear();
    std::string login_url = getLoginUrl();
    if (login_url.empty()) return fail("Call startAuth() first");

    AppletType applet_type = appletGetAppletType();
    if (applet_type != AppletType_Application &&
        applet_type != AppletType_SystemApplication) {
        return fail("PSN login requires application mode. Launch Homebrew Menu "
                    "with Title Override, then reopen LunarNX");
    }

    WebCommonConfig config{};
    Result rc = webPageCreate(&config, login_url.c_str());
    if (R_FAILED(rc)) {
        return fail("Unable to open the Switch browser (webPageCreate " +
                    std::to_string(rc) + ")");
    }

    // Atmosphere supplies the HBL whitelist from /atmosphere/hbl_html. The
    // fixed-size URL, whitelist, and callback TLVs cannot all fit in libnx's
    // 0x2000-byte WebApplet argument storage.
    rc = webConfigSetCallbackUrl(&config, kRedirectUri);
    if (R_FAILED(rc)) {
        return fail("Switch browser callback URL is unavailable (" +
                    std::to_string(rc) + ")");
    }

    WebCommonReply reply{};
    rc = webConfigShow(&config, &reply);
    if (R_FAILED(rc)) {
        return fail("Switch browser failed (webConfigShow " + std::to_string(rc) +
                    "). Confirm Atmosphere hbl_html is installed");
    }

    WebExitReason reason = WebExitReason_UnknownE;
    rc = webReplyGetExitReason(&reply, &reason);
    if (R_FAILED(rc) || reason != WebExitReason_LastUrl) {
        const bool cancelled = reason == WebExitReason_ExitButton ||
            reason == WebExitReason_BackButton;
        return fail(cancelled
                        ? "Login cancelled"
                        : "Switch browser closed before Sony returned a login code",
                    {}, cancelled ? PsnAuthErrorKind::Cancelled
                                  : PsnAuthErrorKind::Fatal);
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

bool PsnAuthManager::submitRedirectUrl(const std::string& input, StateCallback cb,
                                       CancelCallback cancel) {
    if (cancel && cancel()) return false;
    std::string code = extractPsnAuthorizationCode(input);
    if (code.empty()) {
        if (cancel && cancel()) return false;
        return fail("Enter the full Sony redirect URL or authorization code", cb);
    }
    return exchangeCodeForToken(code, std::move(cb), std::move(cancel));
}

bool PsnAuthManager::requestToken(const std::map<std::string, std::string>& params,
                                  bool preserve_refresh_token,
                                  CancelCallback cancel,
                                  RequestTicket ticket) {
    api::HttpClient http;
    std::map<std::string, std::string> headers{
        {"Authorization", basicAuthorization()},
        {"Content-Type", "application/x-www-form-urlencoded"},
    };

    CancelCallback request_cancel = [this, cancel, ticket]() {
        return requestCancelled(cancel, ticket);
    };
    auto response = http.post(kTokenUrl, formEncode(params), headers,
                              request_cancel);
    if (!request_generation_.isCurrent(ticket)) return false;
    if (request_cancel()) {
        return failRequest("PSN token request cancelled",
                           PsnAuthErrorKind::Cancelled, ticket);
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        PsnAuthErrorKind kind = PsnAuthErrorKind::Fatal;
        const bool cancelled = request_cancel();
        if (cancelled) {
            kind = PsnAuthErrorKind::Cancelled;
        } else if (response.network_error || response.status_code == 408 ||
            response.status_code == 429 || response.status_code >= 500) {
            kind = PsnAuthErrorKind::Transient;
        } else if (response.status_code == 400 || response.status_code == 401) {
            cJSON* error_root = cJSON_Parse(response.body.c_str());
            std::string oauth_error = error_root ? jsonString(error_root, "error") : std::string{};
            if (error_root) cJSON_Delete(error_root);
            if (oauth_error == "invalid_grant") kind = PsnAuthErrorKind::SessionExpired;
        }
        std::string detail = cancelled ? "cancelled"
            : response.network_error ? response.error_message
            : "HTTP " + std::to_string(response.status_code);
        return failRequest("PSN token request failed: " + detail, kind, ticket);
    }

    cJSON* root = cJSON_Parse(response.body.c_str());
    if (!root) {
        return failRequest("PSN token request returned invalid JSON",
                           PsnAuthErrorKind::Fatal, ticket);
    }

    std::string access_token = jsonString(root, "access_token");
    std::string refresh_token = jsonString(root, "refresh_token");
    int expires_in = jsonInt(root, "expires_in");
    cJSON_Delete(root);

    if (access_token.empty()) {
        return failRequest("PSN token response has no access_token",
                           PsnAuthErrorKind::Fatal, ticket);
    }
    if (expires_in <= 0) expires_in = 3600;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requestCancelled(cancel, ticket)) return false;
        access_token_ = std::move(access_token);
        if (!refresh_token.empty() || !preserve_refresh_token) {
            refresh_token_ = std::move(refresh_token);
        }
        if (!preserve_refresh_token) {
            // An authorization-code grant may belong to a different account.
            // Never pair its new token with identity retained from the old
            // session if the subsequent account lookup is cancelled.
            account_id_.clear();
            online_id_.clear();
        }
        expires_in_ = expires_in;
        expires_at_ms_ = nowMilliseconds() + static_cast<uint64_t>(expires_in) * 1000ULL;
        error_.clear();
        error_kind_ = PsnAuthErrorKind::None;
    }
    return true;
}

bool PsnAuthManager::exchangeCodeForToken(const std::string& code, StateCallback cb,
                                          CancelCallback cancel) {
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    const RequestTicket ticket = request_generation_.capture();
    if (requestCancelled(cancel, ticket)) return false;
    state_.store(PsnAuthState::ExchangingCode);
    std::map<std::string, std::string> params{
        {"grant_type", "authorization_code"},
        {"code", code},
        {"redirect_uri", kRedirectUri},
        {"scope", kScope},
    };
    if (!requestToken(params, false, cancel, ticket)) {
        if (!request_generation_.isCurrent(ticket)) return false;
        if (cb) cb(PsnAuthState::Error, getAuthError());
        return false;
    }
    if (!fetchAccountId(cancel, ticket)) {
        if (!request_generation_.isCurrent(ticket)) return false;
        if (cb) cb(PsnAuthState::Error, getAuthError());
        return false;
    }

    if (requestCancelled(cancel, ticket) || !markAuthenticated(ticket)) {
        return false;
    }
    diagnosticLog("psn-auth", "Authenticated with PSN account ID");
    if (cb && request_generation_.isCurrent(ticket)) {
        cb(PsnAuthState::Authenticated, {});
    }
    return true;
}

bool PsnAuthManager::refreshAccessToken(CancelCallback cancel,
                                        RequestTicket ticket) {
    std::string refresh_token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        refresh_token = refresh_token_;
    }
    if (refresh_token.empty()) {
        return failRequest("PSN session has no refresh token",
                           PsnAuthErrorKind::SessionExpired, ticket);
    }

    state_.store(PsnAuthState::ExchangingCode);
    std::map<std::string, std::string> params{
        {"grant_type", "refresh_token"},
        {"refresh_token", refresh_token},
        {"redirect_uri", kRedirectUri},
        {"scope", kScope},
    };
    if (!requestToken(params, true, cancel, ticket)) return false;

    // account_id is required by Remote Play. online_id is display-only and a
    // missing display name must not reject an otherwise valid stream token.
    if (getAccountId().empty() && !fetchAccountId(cancel, ticket)) {
        return false;
    }
    if (requestCancelled(cancel, ticket) || !markAuthenticated(ticket)) {
        return false;
    }
    diagnosticLog("psn-auth", "PSN access token refreshed");
    return true;
}

bool PsnAuthManager::refreshIdentity(CancelCallback cancel) {
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    const RequestTicket ticket = request_generation_.capture();
    return fetchAccountId(std::move(cancel), ticket);
}

bool PsnAuthManager::ensureValidToken(StateCallback cb, bool* refreshed,
                                      CancelCallback cancel) {
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    const RequestTicket ticket = request_generation_.capture();
    if (refreshed) *refreshed = false;
    if (hasValidToken()) {
        if (getAccountId().empty() && !fetchAccountId(cancel, ticket)) {
            if (cb) cb(PsnAuthState::Error, getAuthError());
            return false;
        }
        if (requestCancelled(cancel, ticket) || !markAuthenticated(ticket)) {
            return false;
        }
        if (cb && request_generation_.isCurrent(ticket)) {
            cb(PsnAuthState::Authenticated, {});
        }
        return true;
    }
    if (!refreshAccessToken(cancel, ticket)) {
        if (!request_generation_.isCurrent(ticket)) return false;
        if (cb) cb(PsnAuthState::Error, getAuthError());
        return false;
    }
    if (refreshed) *refreshed = true;
    if (cb && request_generation_.isCurrent(ticket)) {
        cb(PsnAuthState::Authenticated, {});
    }
    return true;
}

bool PsnAuthManager::refreshToken(StateCallback cb, CancelCallback cancel) {
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    const RequestTicket ticket = request_generation_.capture();
    if (!refreshAccessToken(cancel, ticket)) {
        if (!request_generation_.isCurrent(ticket)) return false;
        if (cb) cb(PsnAuthState::Error, getAuthError());
        return false;
    }
    if (cb && request_generation_.isCurrent(ticket)) {
        cb(PsnAuthState::Authenticated, {});
    }
    return true;
}

bool PsnAuthManager::fetchAccountId(CancelCallback cancel,
                                    RequestTicket ticket) {
    std::string token = getAccessToken();
    if (token.empty()) {
        return failRequest("PSN account lookup requires an access token",
                           PsnAuthErrorKind::SessionExpired, ticket);
    }

    api::HttpClient http;
    std::map<std::string, std::string> headers{{"Authorization", basicAuthorization()}};
    CancelCallback request_cancel = [this, cancel, ticket]() {
        return requestCancelled(cancel, ticket);
    };
    auto response = http.getSensitive(
        std::string(kTokenUrl) + "/" + urlEncode(token), kTokenUrl, headers,
        request_cancel);
    if (!request_generation_.isCurrent(ticket)) return false;
    if (request_cancel()) {
        return failRequest("PSN account lookup cancelled",
                           PsnAuthErrorKind::Cancelled, ticket);
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        const bool cancelled = request_cancel();
        PsnAuthErrorKind kind = PsnAuthErrorKind::Fatal;
        if (cancelled) {
            kind = PsnAuthErrorKind::Cancelled;
        } else if (response.network_error || response.status_code == 408 ||
                   response.status_code == 429 ||
                   response.status_code >= 500) {
            kind = PsnAuthErrorKind::Transient;
        }
        std::string detail = cancelled ? "cancelled"
            : response.network_error ? response.error_message
            : "HTTP " + std::to_string(response.status_code);
        return failRequest("PSN account lookup failed: " + detail, kind,
                           ticket);
    }

    cJSON* root = cJSON_Parse(response.body.c_str());
    if (!root) {
        return failRequest("PSN account lookup returned invalid JSON",
                           PsnAuthErrorKind::Fatal, ticket);
    }
    std::string user_id = jsonString(root, "user_id");
    std::string online_id = jsonString(root, "online_id");
    cJSON_Delete(root);
    if (user_id.empty()) {
        return failRequest("PSN account lookup returned no user ID",
                           PsnAuthErrorKind::Fatal, ticket);
    }

    try {
        size_t consumed = 0;
        unsigned long long uid = std::stoull(user_id, &consumed, 10);
        if (consumed != user_id.size()) {
            return failRequest("PSN account lookup returned an invalid user ID",
                               PsnAuthErrorKind::Fatal, ticket);
        }
        uint8_t bytes[8]{};
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = static_cast<uint8_t>((uid >> (i * 8)) & 0xff);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (requestCancelled(cancel, ticket)) return false;
        account_id_ = base64Encode(bytes, sizeof(bytes));
        online_id_ = online_id;
        error_.clear();
        error_kind_ = PsnAuthErrorKind::None;
        return true;
    } catch (...) {
        return failRequest("PSN account lookup returned an invalid user ID",
                           PsnAuthErrorKind::Fatal, ticket);
    }
}

bool PsnAuthManager::requestCancelled(const CancelCallback& cancel,
                                      RequestTicket ticket) const {
    return !request_generation_.isCurrent(ticket) || (cancel && cancel());
}

bool PsnAuthManager::failRequest(const std::string& message,
                                 PsnAuthErrorKind kind,
                                 RequestTicket ticket,
                                 StateCallback cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!request_generation_.isCurrent(ticket)) return false;
        error_ = message;
        error_kind_ = kind;
        state_.store(PsnAuthState::Error);
    }
    diagnosticLog("psn-auth", "%s", message.c_str());
    if (cb && request_generation_.isCurrent(ticket)) {
        cb(PsnAuthState::Error, message);
    }
    return false;
}

bool PsnAuthManager::markAuthenticated(RequestTicket ticket) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!request_generation_.isCurrent(ticket)) return false;
    error_.clear();
    error_kind_ = PsnAuthErrorKind::None;
    state_.store(PsnAuthState::Authenticated);
    return true;
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
    request_generation_.invalidate();
    const RequestTicket ticket = request_generation_.capture();
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    if (!request_generation_.isCurrent(ticket)) return false;
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
        if (!request_generation_.isCurrent(ticket)) return false;
        access_token_ = std::move(access_token);
        refresh_token_ = std::move(refresh_token);
        account_id_ = std::move(account_id);
        online_id_ = std::move(online_id);
        if (duid.size() == kDuidLength) duid_ = std::move(duid);
        expires_in_ = expires_in;
        expires_at_ms_ = expires_at;
        error_.clear();
        error_kind_ = PsnAuthErrorKind::None;
        const bool valid = !access_token_.empty() &&
            (expires_at_ms_ == 0 ||
             expires_at_ms_ > nowMilliseconds() + kRemoteSessionTokenMarginMs);
        state_.store(valid ? PsnAuthState::Authenticated : PsnAuthState::Idle);
    }
    return true;
}

bool PsnAuthManager::saveToken(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (access_token_.empty() && refresh_token_.empty()) return false;
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
    std::string temp_path = path + ".tmp";
    FILE* file = std::fopen(temp_path.c_str(), "w");
    if (!file) {
        std::free(json);
        return false;
    }
    bool ok = std::fputs(json, file) >= 0;
    ok = std::fflush(file) == 0 && ok;
    ok = std::fclose(file) == 0 && ok;
    std::free(json);
    if (!ok) {
        std::remove(temp_path.c_str());
        return false;
    }
    std::string backup_path = path + ".bak";
    std::remove(backup_path.c_str());
    errno = 0;
    const bool had_existing = std::rename(path.c_str(), backup_path.c_str()) == 0;
    if (!had_existing && errno != ENOENT) {
        diagnosticLog("psn-auth", "PSN token backup failed errno=%d", errno);
        std::remove(temp_path.c_str());
        return false;
    }
    if (std::rename(temp_path.c_str(), path.c_str()) != 0) {
        const int replace_errno = errno;
        if (had_existing) std::rename(backup_path.c_str(), path.c_str());
        std::remove(temp_path.c_str());
        diagnosticLog("psn-auth", "PSN token replace failed errno=%d", replace_errno);
        return false;
    }
    if (had_existing) std::remove(backup_path.c_str());
    return true;
}

void PsnAuthManager::signOut() {
    request_generation_.invalidate();
    std::lock_guard<std::mutex> lock(mutex_);
    access_token_.clear();
    refresh_token_.clear();
    account_id_.clear();
    online_id_.clear();
    login_url_.clear();
    error_.clear();
    error_kind_ = PsnAuthErrorKind::None;
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

PsnAuthErrorKind PsnAuthManager::getAuthErrorKind() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_kind_;
}

std::string PsnAuthManager::getLoginUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return login_url_;
}

} // namespace lunar::ps

#endif
