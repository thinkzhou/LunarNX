#include "auth_manager.h"
#include "xbox_auth_errors.h"
#include "../api/api_constants.h"
#include "../diagnostics.h"
#include <cJSON.h>
#include <cstdio>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace lunar::auth {

using namespace lunar::api::constants;

// MSAL device code client_id (from Greenlight/xbox-xcloud-player)
static constexpr const char* MSAL_CLIENT_ID = "1f907974-e22b-4810-a9de-d9647380c97e";
static constexpr const char* MSAL_SCOPE = "xboxlive.signin openid profile offline_access";
static constexpr const char* DEVICECODE_URL = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
static constexpr const char* TOKEN_URL = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
static constexpr const char* LIVE_TOKEN_URL = "https://login.live.com/oauth20_token.srf";
static constexpr const char* XCLOUD_TRANSFER_SCOPE =
    "service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN";

namespace {

static const char* json_string(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return (item && cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

static std::string json_value_string(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (!item) return "";
    if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
    if (cJSON_IsNumber(item)) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(0) << item->valuedouble;
        return out.str();
    }
    return "";
}

static void log_msal_account_claims(cJSON* token_root) {
    const char* id_token = json_string(token_root, "id_token");
    const char* user_id = json_string(token_root, "user_id");
    lunar::diagnosticLog("auth", "Microsoft token account id_token=%s user_id=%s",
                         id_token && id_token[0] != '\0' ? "present" : "missing",
                         user_id && user_id[0] != '\0' ? "present" : "missing");
}

static void extract_xbox_display_claims(cJSON* root, const char* label,
                                        std::string* user_hash,
                                        std::string* gamertag) {
    cJSON* dc = cJSON_GetObjectItem(root, "DisplayClaims");
    cJSON* xui = dc ? cJSON_GetObjectItem(dc, "xui") : nullptr;
    if (!xui || !cJSON_IsArray(xui) || cJSON_GetArraySize(xui) <= 0) {
        lunar::diagnosticLog("auth", "%s DisplayClaims xui missing", label);
        return;
    }

    cJSON* xui0 = cJSON_GetArrayItem(xui, 0);
    const char* uhs = json_string(xui0, "uhs");
    const char* gtg = json_string(xui0, "gtg");
    const char* xid = json_string(xui0, "xid");
    const char* agg = json_string(xui0, "agg");
    if (user_hash && user_hash->empty() && uhs[0] != '\0') *user_hash = uhs;
    if (gamertag && gamertag->empty() && gtg[0] != '\0') *gamertag = gtg;

    lunar::diagnosticLog("auth", "%s Xbox claims uhs=%s gamertag=%s xid=%s age_group=%s",
                         label,
                         uhs[0] != '\0' ? "present" : "missing",
                         gtg[0] != '\0' ? "present" : "missing",
                         xid[0] != '\0' ? "present" : "missing",
                         agg[0] != '\0' ? "present" : "missing");
}

static void log_auth_failure_body(const char* label, int status, const std::string& body) {
    std::string xerr;
    std::string message;
    std::string error;
    std::string error_description;

    cJSON* root = cJSON_Parse(body.c_str());
    if (root) {
        xerr = json_value_string(root, "XErr");
        message = json_value_string(root, "Message");
        error = json_value_string(root, "error");
        error_description = json_value_string(root, "error_description");
        cJSON_Delete(root);
    }

    lunar::diagnosticLog(
        "auth",
        "%s failed status=%d XErr=%s Message=%s error=%s error_description=%s",
        label,
        status,
        xerr.c_str(),
        message.c_str(),
        error.c_str(),
        error_description.c_str());
}

} // namespace

// =============================================================================
// AuthManager
// =============================================================================
AuthManager::AuthManager(api::HttpClient& http) : http_(http) {}
AuthManager::~AuthManager() = default;

void AuthManager::setState(AuthState state, const std::string& info) {
    state_ = state;
    if (state_callback_) state_callback_(state, info);
}

// =============================================================================
// Start MSAL Device Code Flow
// =============================================================================
bool AuthManager::startDeviceCodeAuth() {
    clearInMemoryTokens();
    last_error_.clear();
    poll_interval_ = 5;
    device_code_expires_in_ = 900;
    setState(AuthState::Authenticating, "Requesting device code...");

    std::ostringstream body;
    body << "client_id=" << MSAL_CLIENT_ID
         << "&scope=xboxlive.signin%20openid%20profile%20offline_access";

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/x-www-form-urlencoded";

    auto resp = http_.post(DEVICECODE_URL, body.str(), headers);

    if (resp.status_code != 200) {
        last_error_ = resp.network_error
            ? "Network error. Check WiFi and try again."
            : "Failed to get sign-in code. HTTP " + std::to_string(resp.status_code) + ".";
        fprintf(stderr, "[auth] Device code request failed: HTTP %d\n", resp.status_code);
        lunar::diagnosticLog("auth", "Device code request failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
        setState(AuthState::Error, last_error_);
        return false;
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) {
        last_error_ = "Failed to parse sign-in response.";
        lunar::diagnosticLog("auth", "Device code response parse failed");
        setState(AuthState::Error, last_error_);
        return false;
    }

    cJSON* dc = cJSON_GetObjectItem(root, "device_code");
    cJSON* uc = cJSON_GetObjectItem(root, "user_code");
    cJSON* vu = cJSON_GetObjectItem(root, "verification_uri");
    cJSON* msg = cJSON_GetObjectItem(root, "message");
    cJSON* interval = cJSON_GetObjectItem(root, "interval");
    cJSON* expires_in = cJSON_GetObjectItem(root, "expires_in");

    if (!dc || !uc || !vu) {
        cJSON_Delete(root);
        last_error_ = "Microsoft sign-in response was incomplete.";
        lunar::diagnosticLog("auth", "Device code response missing fields");
        setState(AuthState::Error, last_error_);
        return false;
    }

    device_code_ = dc->valuestring;
    user_code_ = uc->valuestring;
    verification_uri_ = vu->valuestring;
    auth_message_ = msg ? msg->valuestring : "";
    if (interval && cJSON_IsNumber(interval)) poll_interval_ = interval->valueint;
    if (expires_in && cJSON_IsNumber(expires_in)) device_code_expires_in_ = expires_in->valueint;

    cJSON_Delete(root);

    setState(AuthState::WaitingForDeviceCode,
             "Go to " + verification_uri_ + " and enter code: " + user_code_);

    return true;
}

// =============================================================================
// Poll for token
// =============================================================================
bool AuthManager::pollForToken() {
    return pollForTokenResult() == DeviceCodePollResult::Authenticated;
}

DeviceCodePollResult AuthManager::pollForTokenResult() {
    if (device_code_.empty()) {
        last_error_ = "No active sign-in code. Press Start for a new one.";
        setState(AuthState::Error, last_error_);
        return DeviceCodePollResult::Error;
    }

    std::ostringstream body;
    body << "grant_type=urn:ietf:params:oauth:grant-type:device_code"
         << "&client_id=" << MSAL_CLIENT_ID
         << "&device_code=" << device_code_;

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/x-www-form-urlencoded";

    auto resp = http_.post(TOKEN_URL, body.str(), headers);

    if (resp.status_code == 200) {
        cJSON* root = cJSON_Parse(resp.body.c_str());
        if (!root) {
            last_error_ = "Failed to parse Microsoft token response.";
            lunar::diagnosticLog("auth", "Token response parse failed");
            return DeviceCodePollResult::Error;
        }

        cJSON* at = cJSON_GetObjectItem(root, "access_token");
        cJSON* rt = cJSON_GetObjectItem(root, "refresh_token");
        cJSON* ei = cJSON_GetObjectItem(root, "expires_in");

        if (at && cJSON_IsString(at)) msal_access_token_ = at->valuestring;
        if (rt && cJSON_IsString(rt)) msal_refresh_token_ = rt->valuestring;
        int expires_in = ei ? ei->valueint : 3600;
        log_msal_account_claims(root);

        cJSON_Delete(root);

        if (msal_access_token_.empty()) {
            last_error_ = "Microsoft token response did not include an access token.";
            lunar::diagnosticLog("auth", "Token response missing access token");
            return DeviceCodePollResult::Error;
        }

        tokens_.setUserToken(msal_access_token_, msal_refresh_token_, "", expires_in);

        setState(AuthState::Authenticating, "Getting Xbox tokens...");
        if (stepGetStreamingTokens()) {
            last_error_.clear();
            setState(AuthState::Authenticated, gamertag_);
            return DeviceCodePollResult::Authenticated;
        } else {
            if (last_error_.empty()) {
                last_error_ = "Failed to get Xbox streaming tokens.";
            }
            setState(AuthState::Error, last_error_);
            return DeviceCodePollResult::Error;
        }
    }

    std::string error_code;
    if (cJSON* root = cJSON_Parse(resp.body.c_str())) {
        cJSON* error = cJSON_GetObjectItem(root, "error");
        if (error && cJSON_IsString(error)) {
            error_code = error->valuestring;
        }
        cJSON_Delete(root);
    }

    if (error_code == "authorization_pending") {
        return DeviceCodePollResult::Pending;
    }
    if (error_code == "slow_down") {
        poll_interval_ += 5;
        setState(AuthState::WaitingForDeviceCode,
                 "Microsoft asked to slow down. Waiting before retry...");
        return DeviceCodePollResult::SlowDown;
    }
    if (error_code == "authorization_declined") {
        last_error_ = "Sign-in was declined. Press Start to try again.";
        lunar::diagnosticLog("auth", "Device code sign-in declined");
        setState(AuthState::Error, last_error_);
        return DeviceCodePollResult::Declined;
    }
    if (error_code == "expired_token") {
        last_error_ = "Code expired. Press Start for a new one.";
        lunar::diagnosticLog("auth", "Device code expired");
        setState(AuthState::Error, last_error_);
        return DeviceCodePollResult::Expired;
    }

    last_error_ = resp.network_error
        ? "Network error while waiting for sign-in. Check WiFi and try again."
        : "Sign-in failed. HTTP " + std::to_string(resp.status_code) + ". Press Start to try again.";
    fprintf(stderr, "[auth] Token poll error: HTTP %d\n", resp.status_code);
    lunar::diagnosticLog("auth", "Token poll failed status=%d network=%s error=%s",
                         resp.status_code,
                         resp.network_error ? "true" : "false",
                         resp.error_message.c_str());
    log_auth_failure_body("Token poll", resp.status_code, resp.body);
    setState(AuthState::Error, last_error_);
    return DeviceCodePollResult::Error;
}

// =============================================================================
// Common Xbox Live headers (matching Greenlight/xbox-xcloud-player exactly)
// =============================================================================
static std::map<std::string, std::string> xbox_headers() {
    return {
        {"x-xbl-contract-version", "1"},
        {"Cache-Control", "no-cache"},
        {"Content-Type", "application/json"},
        {"Origin", "https://www.xbox.com"},
        {"Referer", "https://www.xbox.com/"},
        {"Accept", "*/*"},
        {"ms-cv", "0"},
        {"User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"},
    };
}

// =============================================================================
// Get Xbox streaming tokens (RPS → XSTS → GSSV)
// =============================================================================
bool AuthManager::stepGetStreamingTokens() {
    // Step 1: RPS Authentication - get Xbox user token
    // Exact match with Greenlight's doXstsAuthentication()
    cJSON* rps_props = cJSON_CreateObject();
    cJSON_AddStringToObject(rps_props, "AuthMethod", "RPS");
    cJSON_AddStringToObject(rps_props, "SiteName", "user.auth.xboxlive.com");
    std::string rps_ticket = "d=" + msal_access_token_;
    cJSON_AddStringToObject(rps_props, "RpsTicket", rps_ticket.c_str());

    cJSON* rps_body = cJSON_CreateObject();
    cJSON_AddItemToObject(rps_body, "Properties", rps_props);
    cJSON_AddStringToObject(rps_body, "RelyingParty", "http://auth.xboxlive.com");
    cJSON_AddStringToObject(rps_body, "TokenType", "JWT");

    char* rps_str = cJSON_PrintUnformatted(rps_body);
    cJSON_Delete(rps_body);

    auto headers = xbox_headers();

    auto resp = http_.post("https://user.auth.xboxlive.com/user/authenticate", rps_str, headers);
    free(rps_str);

    if (resp.status_code != 200) {
        last_error_ = resp.network_error
            ? "Network error while getting Xbox user token. Check WiFi and try again."
            : "Xbox user token request failed. HTTP " + std::to_string(resp.status_code) + ".";
        fprintf(stderr, "[auth] RPS auth failed: HTTP %d\n", resp.status_code);
        lunar::diagnosticLog("auth", "RPS auth failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
        log_auth_failure_body("RPS auth", resp.status_code, resp.body);
        return false;
    }

    cJSON* rps_root = cJSON_Parse(resp.body.c_str());
    if (!rps_root) {
        last_error_ = "Failed to parse Xbox user token response.";
        lunar::diagnosticLog("auth", "RPS auth response parse failed");
        return false;
    }

    cJSON* rps_token_item = cJSON_GetObjectItem(rps_root, "Token");
    std::string xbox_user_token;
    if (rps_token_item && cJSON_IsString(rps_token_item)) {
        xbox_user_token = rps_token_item->valuestring;
    }
    extract_xbox_display_claims(rps_root, "RPS auth", &user_hash_, &gamertag_);
    cJSON_Delete(rps_root);

    if (xbox_user_token.empty()) {
        last_error_ = "Xbox user token response was missing a token.";
        fprintf(stderr, "[auth] RPS token extraction failed\n");
        lunar::diagnosticLog("auth", "RPS auth response missing token");
        return false;
    }

    // Step 2: XSTS Authorization for GSSV (streaming)
    // Exact match with Greenlight's doXstsAuthorization('http://gssv.xboxlive.com/')
    {
        cJSON* xsts_props = cJSON_CreateObject();
        cJSON_AddStringToObject(xsts_props, "SandboxId", SANDBOX);
        cJSON* user_tokens = cJSON_CreateArray();
        cJSON_AddItemToArray(user_tokens, cJSON_CreateString(xbox_user_token.c_str()));
        cJSON_AddItemToObject(xsts_props, "UserTokens", user_tokens);

        cJSON* xsts_body = cJSON_CreateObject();
        cJSON_AddItemToObject(xsts_body, "Properties", xsts_props);
        cJSON_AddStringToObject(xsts_body, "RelyingParty", "http://gssv.xboxlive.com/");
        cJSON_AddStringToObject(xsts_body, "TokenType", "JWT");

        char* xsts_str = cJSON_PrintUnformatted(xsts_body);
        cJSON_Delete(xsts_body);

        headers = xbox_headers();
        resp = http_.post("https://xsts.auth.xboxlive.com/xsts/authorize", xsts_str, headers);
        free(xsts_str);

        if (resp.status_code != 200) {
            last_error_ = resp.network_error
                ? "Network error while authorizing Xbox streaming. Check WiFi and try again."
                : describeXstsFailure(resp.status_code, resp.body);
            fprintf(stderr, "[auth] XSTS GSSV failed: HTTP %d\n", resp.status_code);
            lunar::diagnosticLog("auth", "XSTS GSSV failed status=%d network=%s error=%s",
                                 resp.status_code,
                                 resp.network_error ? "true" : "false",
                                 resp.error_message.c_str());
            log_auth_failure_body("XSTS GSSV", resp.status_code, resp.body);
            return false;
        }

        cJSON* xsts_root = cJSON_Parse(resp.body.c_str());
        if (!xsts_root) {
            last_error_ = "Failed to parse Xbox streaming authorization response.";
            lunar::diagnosticLog("auth", "XSTS GSSV response parse failed");
            return false;
        }

        cJSON* xt = cJSON_GetObjectItem(xsts_root, "Token");
        if (!xt || !cJSON_IsString(xt)) {
            last_error_ = "Xbox streaming authorization response was missing a token.";
            fprintf(stderr, "[auth] XSTS GSSV: Token not found in response\n");
            lunar::diagnosticLog("auth", "XSTS GSSV response missing token");
            cJSON_Delete(xsts_root);
            return false;
        }
        xsts_gssv_token_ = xt->valuestring;

        // Extract user hash + gamertag
        cJSON* dc = cJSON_GetObjectItem(xsts_root, "DisplayClaims");
        if (dc) {
            cJSON* xui = cJSON_GetObjectItem(dc, "xui");
            if (xui && cJSON_IsArray(xui) && cJSON_GetArraySize(xui) > 0) {
                cJSON* xui0 = cJSON_GetArrayItem(xui, 0);
                cJSON* uhs = cJSON_GetObjectItem(xui0, "uhs");
                cJSON* gtg = cJSON_GetObjectItem(xui0, "gtg");
                if (uhs && cJSON_IsString(uhs)) user_hash_ = uhs->valuestring;
                if (gtg && cJSON_IsString(gtg)) gamertag_ = gtg->valuestring;
            }
        }
        extract_xbox_display_claims(xsts_root, "XSTS GSSV", &user_hash_, &gamertag_);
        cJSON_Delete(xsts_root);
    }

    // Step 2b: Get web token for console list API
    // Exact match with Greenlight's doXstsAuthorization('http://xboxlive.com')
    {
        cJSON* wprops = cJSON_CreateObject();
        cJSON_AddStringToObject(wprops, "SandboxId", SANDBOX);
        cJSON* wtokens = cJSON_CreateArray();
        cJSON_AddItemToArray(wtokens, cJSON_CreateString(xbox_user_token.c_str()));
        cJSON_AddItemToObject(wprops, "UserTokens", wtokens);

        cJSON* wbody = cJSON_CreateObject();
        cJSON_AddItemToObject(wbody, "Properties", wprops);
        cJSON_AddStringToObject(wbody, "RelyingParty", "http://xboxlive.com");
        cJSON_AddStringToObject(wbody, "TokenType", "JWT");

        char* wstr = cJSON_PrintUnformatted(wbody);
        cJSON_Delete(wbody);

        headers = xbox_headers();
        resp = http_.post("https://xsts.auth.xboxlive.com/xsts/authorize", wstr, headers);
        free(wstr);

        if (resp.status_code == 200) {
            cJSON* wroot = cJSON_Parse(resp.body.c_str());
            if (wroot) {
                cJSON* wt = cJSON_GetObjectItem(wroot, "Token");
                if (wt && cJSON_IsString(wt)) web_token_ = wt->valuestring;
                // Extract gamertag from web token response
                cJSON* wdc = cJSON_GetObjectItem(wroot, "DisplayClaims");
                if (wdc) {
                    cJSON* wxui = cJSON_GetObjectItem(wdc, "xui");
                    if (wxui && cJSON_IsArray(wxui) && cJSON_GetArraySize(wxui) > 0) {
                        cJSON* wxui0 = cJSON_GetArrayItem(wxui, 0);
                        cJSON* wgtg = cJSON_GetObjectItem(wxui0, "gtg");
                        if (wgtg && cJSON_IsString(wgtg) && gamertag_.empty()) gamertag_ = wgtg->valuestring;
                    }
                }
                extract_xbox_display_claims(wroot, "XSTS web", &user_hash_, &gamertag_);
                cJSON_Delete(wroot);
            }
        } else {
            log_auth_failure_body("XSTS web", resp.status_code, resp.body);
        }
    }

    if (web_token_.empty() || user_hash_.empty()) {
        last_error_ = "Xbox web authorization did not return account details.";
        fprintf(stderr, "[auth] Web token/user hash missing after XSTS authorization\n");
        lunar::diagnosticLog("auth", "Web token/user hash missing after XSTS authorization");
        return false;
    }

    // Step 3: Get GSSV streaming tokens (xhome required, xCloud optional)
    if (!fetchStreamToken(xsts_gssv_token_,
                          OFFERING_XHOME,
                          GSSV_XHOME_LOGIN,
                          &home_token_,
                          true)) {
        return false;
    }
    gssv_token_ = home_token_.gs_token;

    cloud_token_ = {};
    if (!fetchStreamToken(xsts_gssv_token_,
                          OFFERING_XGPUWEB,
                          GSSV_XGPUWEB_LOGIN,
                          &cloud_token_,
                          false)) {
        // Free-to-play fallback, matching XStreaming/Greenlight.
        fetchStreamToken(xsts_gssv_token_,
                         OFFERING_XGPUWEBF2P,
                         GSSV_XGPUWEBF2P_LOGIN,
                         &cloud_token_,
                         false);
    }
    if (cloud_token_.valid()) {
        lunar::diagnosticLog("auth", "Cloud streaming token acquired base=%s",
                             cloud_token_.base_uri.c_str());
        // Don't leave a previous optional-cloud error sticky after success.
        if (last_error_.find("xCloud token request failed") == 0) {
            last_error_.clear();
        }
    } else {
        // Official xCloud offerings returned no token. The most common case is HTTP 403
        // (OfferingAccessDenied) when the account has no Game Pass Ultimate / xCloud rights.
        if (last_error_.find("xCloud token request failed") != 0) {
            last_error_ =
                "xCloud access denied by Xbox (HTTP 403). "
                "Missing xCloud rights or unsupported region "
                "(Game Pass Ultimate required; try force_region_ip if InvalidCountry).";
        } else if (last_error_.find("InvalidCountry") != std::string::npos) {
            last_error_ =
                "xCloud access denied by Xbox (HTTP 403 InvalidCountry). "
                "Your current region is not supported for xCloud. "
                "Try setting force_region_ip (e.g. US 4.2.2.2) like XStreaming, "
                "or use a supported country / Game Pass Ultimate account. "
                "Detail: " + last_error_;
        } else if (last_error_.find("HTTP 403") != std::string::npos) {
            last_error_ =
                "xCloud access denied by Xbox (HTTP 403). "
                "This Microsoft account may lack xCloud rights "
                "(Game Pass Ultimate required, or region/subscription unavailable). "
                "Detail: " + last_error_;
        }
        lunar::diagnosticLog("auth", "Cloud streaming token unavailable: %s", last_error_.c_str());
    }

    // Persist user info
    {
        SisuTokenData sisu;
        sisu.user_hash = user_hash_;
        sisu.gamertag = gamertag_;
        tokens_.setSisuToken(sisu);
    }

    return hasUsableStreamingTokens();
}

// =============================================================================
// Fetch one GSSV streaming token (xhome / xgpuweb / xgpuwebf2p)
// =============================================================================
bool AuthManager::fetchStreamToken(const std::string& xsts_token,
                                   const std::string& offering_id,
                                   const std::string& login_url,
                                   StreamingToken* out_token,
                                   bool required) {
    if (!out_token) {
        return false;
    }
    *out_token = {};

    cJSON* gbody = cJSON_CreateObject();
    cJSON_AddStringToObject(gbody, "token", xsts_token.c_str());
    cJSON_AddStringToObject(gbody, "offeringId", offering_id.c_str());

    char* gstr = cJSON_PrintUnformatted(gbody);
    cJSON_Delete(gbody);

    std::map<std::string, std::string> gssv_headers;
    gssv_headers["Content-Type"] = "application/json";
    gssv_headers["Cache-Control"] = "no-store, must-revalidate, no-cache";
    gssv_headers["x-gssv-client"] = "XboxComBrowser";
    // XStreaming/Greenlight: spoof client region for xCloud InvalidCountry.
    if (!force_region_ip_.empty()) {
        gssv_headers["x-forwarded-for"] = force_region_ip_;
        lunar::diagnosticLog("auth", "GSSV login offering=%s x-forwarded-for=%s",
                             offering_id.c_str(), force_region_ip_.c_str());
    }

    auto resp = http_.post(login_url, gstr, gssv_headers);
    free(gstr);

    if (resp.status_code != 200) {
        if (required) {
            last_error_ = resp.network_error
                ? "Network error while getting Xbox streaming token. Check WiFi and try again."
                : "Xbox streaming token request failed. HTTP " + std::to_string(resp.status_code) + ".";
            fprintf(stderr, "[auth] GSSV token(%s) failed: HTTP %d\n",
                    offering_id.c_str(), resp.status_code);
            lunar::diagnosticLog("auth", "GSSV token failed offering=%s status=%d network=%s error=%s",
                                 offering_id.c_str(),
                                 resp.status_code,
                                 resp.network_error ? "true" : "false",
                                 resp.error_message.c_str());
            log_auth_failure_body(("GSSV token " + offering_id).c_str(), resp.status_code, resp.body);
        } else {
            // Keep a human-readable note for xCloud 403 so UI can surface it.
            std::string detail;
            cJSON* err_root = cJSON_Parse(resp.body.c_str());
            if (err_root) {
                cJSON* code = cJSON_GetObjectItem(err_root, "code");
                cJSON* message = cJSON_GetObjectItem(err_root, "message");
                if (code && cJSON_IsString(code) && code->valuestring) detail = code->valuestring;
                if (message && cJSON_IsString(message) && message->valuestring && message->valuestring[0]) {
                    if (!detail.empty()) detail += ": ";
                    detail += message->valuestring;
                }
                cJSON_Delete(err_root);
            }
            if (detail.empty() && !resp.body.empty()) {
                detail = resp.body.substr(0, 120);
            }
            lunar::diagnosticLog("auth",
                                 "Optional GSSV token skipped offering=%s status=%d detail=%s",
                                 offering_id.c_str(), resp.status_code, detail.c_str());
            // Stash for callers when both cloud offerings fail.
            last_error_ = "xCloud token request failed (" + offering_id + ") HTTP " +
                          std::to_string(resp.status_code) +
                          (detail.empty() ? "" : (": " + detail));
        }
        return false;
    }

    cJSON* groot = cJSON_Parse(resp.body.c_str());
    if (!groot) {
        if (required) {
            last_error_ = "Failed to parse Xbox streaming token response.";
            lunar::diagnosticLog("auth", "GSSV token response parse failed offering=%s",
                                 offering_id.c_str());
        }
        return false;
    }

    cJSON* gs = cJSON_GetObjectItem(groot, "gsToken");
    if (!gs || !cJSON_IsString(gs) || !gs->valuestring) {
        if (required) {
            last_error_ = "Xbox streaming token response was missing gsToken.";
            fprintf(stderr, "[auth] GSSV(%s): gsToken not found in response\n", offering_id.c_str());
            lunar::diagnosticLog("auth", "GSSV token response missing gsToken offering=%s",
                                 offering_id.c_str());
        }
        cJSON_Delete(groot);
        return false;
    }

    out_token->gs_token = gs->valuestring;

    cJSON* duration = cJSON_GetObjectItem(groot, "durationInSeconds");
    if (duration && cJSON_IsNumber(duration)) {
        out_token->duration_seconds = duration->valueint;
    }

    // Prefer default region baseUri from offeringSettings.regions[].
    cJSON* offering = cJSON_GetObjectItem(groot, "offeringSettings");
    cJSON* regions = offering ? cJSON_GetObjectItem(offering, "regions") : nullptr;
    if (regions && cJSON_IsArray(regions)) {
        const int count = cJSON_GetArraySize(regions);
        std::string default_uri;
        std::string first_uri;
        for (int i = 0; i < count; ++i) {
            cJSON* region = cJSON_GetArrayItem(regions, i);
            if (!region) continue;
            cJSON* base = cJSON_GetObjectItem(region, "baseUri");
            if (!base || !cJSON_IsString(base) || !base->valuestring) continue;
            if (first_uri.empty()) first_uri = base->valuestring;
            cJSON* is_default = cJSON_GetObjectItem(region, "isDefault");
            if (is_default && cJSON_IsTrue(is_default)) {
                default_uri = base->valuestring;
                break;
            }
        }
        out_token->base_uri = !default_uri.empty() ? default_uri : first_uri;
    }

    cJSON_Delete(groot);
    lunar::diagnosticLog("auth", "GSSV token acquired offering=%s base=%s duration=%d",
                         offering_id.c_str(),
                         out_token->base_uri.c_str(),
                         out_token->duration_seconds);
    return out_token->valid();
}

// =============================================================================
// Token refresh (MSAL refresh_token → new access_token → new Xbox tokens)
// =============================================================================
static std::string urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex << std::uppercase;
    for (char c : value) {
        if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~')
            escaped << c;
        else
            escaped << '%' << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
    }
    return escaped.str();
}

bool AuthManager::refreshTokensIfNeeded() {
    if (!shouldRefreshTokens()) return hasUsableStreamingTokens();
    if (msal_refresh_token_.empty()) return hasUsableStreamingTokens();

    auto now = std::chrono::steady_clock::now();
    if (last_refresh_attempt_.time_since_epoch().count() != 0 &&
        now - last_refresh_attempt_ < std::chrono::minutes(1)) {
        return false;
    }
    last_refresh_attempt_ = now;

    std::ostringstream body;
    body << "grant_type=refresh_token"
         << "&client_id=" << urlEncode(MSAL_CLIENT_ID)
         << "&refresh_token=" << urlEncode(msal_refresh_token_)
         << "&scope=" << urlEncode(MSAL_SCOPE);

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/x-www-form-urlencoded";

    auto resp = http_.post(TOKEN_URL, body.str(), headers);

    if (resp.status_code != 200) {
        last_error_ = resp.network_error
            ? "Network error while refreshing sign-in. Check WiFi and try again."
            : "Saved sign-in refresh failed. HTTP " + std::to_string(resp.status_code) + ".";
        fprintf(stderr, "[auth] Token refresh failed: HTTP %d\n", resp.status_code);
        lunar::diagnosticLog("auth", "Token refresh failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
        return false;
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) {
        last_error_ = "Failed to parse saved sign-in refresh response.";
        lunar::diagnosticLog("auth", "Token refresh response parse failed");
        return false;
    }

    cJSON* at = cJSON_GetObjectItem(root, "access_token");
    cJSON* rt = cJSON_GetObjectItem(root, "refresh_token");
    cJSON* ei = cJSON_GetObjectItem(root, "expires_in");

    if (at && cJSON_IsString(at)) msal_access_token_ = at->valuestring;
    if (rt && cJSON_IsString(rt)) msal_refresh_token_ = rt->valuestring;
    int expires_in = ei ? ei->valueint : 3600;
    log_msal_account_claims(root);

    cJSON_Delete(root);

    if (msal_access_token_.empty()) {
        last_error_ = "Saved sign-in refresh did not return an access token.";
        lunar::diagnosticLog("auth", "Token refresh response missing access token");
        return false;
    }

    tokens_.setUserToken(msal_access_token_, msal_refresh_token_, "", expires_in);

    // Re-derive Xbox streaming tokens with the new MSAL token
    if (!stepGetStreamingTokens()) {
        fprintf(stderr, "[auth] Token refresh: Xbox token derivation failed\n");
        lunar::diagnosticLog("auth", "Token refresh Xbox token derivation failed: %s",
                             last_error_.c_str());
        return false;
    }

    fprintf(stderr, "[auth] Token refresh successful cloud=%s\n",
            cloud_token_.valid() ? "yes" : "no");
    lunar::diagnosticLog("auth", "Token refresh successful cloud=%s base=%s",
                         cloud_token_.valid() ? "yes" : "no",
                         cloud_token_.base_uri.c_str());
    last_error_.clear();
    return true;
}

bool AuthManager::shouldRefreshTokens() const {
    const auto& d = tokens_.data();
    if (msal_refresh_token_.empty()) return false;
    if (!hasUsableStreamingTokens()) return true;
    // Older token.json files may only have home gssv_token. Re-derive so xCloud
    // (xgpuweb / xgpuwebf2p) is populated when the account supports it.
    if (!cloud_token_.valid()) return true;
    if (d.expires_at_ms == 0) return true;
    auto now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    constexpr uint64_t kRefreshLeadMs = 5ULL * 60ULL * 1000ULL;
    return d.expires_at_ms <= now_ms + kRefreshLeadMs;
}

// =============================================================================
// Token accessors
// =============================================================================
bool AuthManager::hasUsableStreamingTokens() const {
    return !gssv_token_.empty() && !web_token_.empty() && !user_hash_.empty();
}

bool AuthManager::isAuthenticated() const { return hasUsableStreamingTokens(); }
bool AuthManager::hasSavedCredentials() const {
    return !msal_refresh_token_.empty() || hasUsableStreamingTokens();
}
std::string AuthManager::getGssvToken() const { return gssv_token_; }

StreamingToken AuthManager::getHomeStreamingToken() const {
    if (home_token_.valid()) return home_token_;
    StreamingToken token;
    token.gs_token = gssv_token_;
    return token;
}

StreamingToken AuthManager::getCloudStreamingToken() const {
    return cloud_token_;
}

bool AuthManager::hasCloudAccess() const {
    return cloud_token_.valid();
}

void AuthManager::setForceRegionIp(const std::string& ip) {
    force_region_ip_ = ip;
    lunar::diagnosticLog("auth", "force_region_ip set=%s",
                         force_region_ip_.empty() ? "(default)" : force_region_ip_.c_str());
    // Region change invalidates any previously denied/cached cloud decision.
    cloud_token_ = {};
    last_refresh_attempt_ = {};
}

bool AuthManager::refreshStreamingTokens(bool force) {
    if (msal_refresh_token_.empty()) {
        return hasUsableStreamingTokens();
    }
    if (force) {
        // Cloud /connect uses MSAL access token ("lpt"). Even if gsToken is still
        // valid, an expired MSAL token causes MsaVeto / UserNeedsToSignInAgainWithMSA.
        last_refresh_attempt_ = {};
        tokens_.data().expires_at_ms = 1; // force shouldRefreshTokens() true
        cloud_token_ = {};
    }
    if (!refreshTokensIfNeeded() && !hasUsableStreamingTokens()) {
        return false;
    }
    return hasUsableStreamingTokens();
}

std::string AuthManager::getXcloudTransferToken(bool force_refresh) {
    // XStreaming/Greenlight: exchange refresh_token for a cloud console transfer
    // token (lpt) used as userToken in POST .../sessions/cloud/{id}/connect.
    if (msal_refresh_token_.empty()) {
        last_error_ = "No refresh token available for xCloud connect (lpt).";
        lunar::diagnosticLog("auth", "getXcloudTransferToken missing refresh_token");
        return "";
    }

    // Cache briefly unless forced.
    static std::string cached_lpt;
    static std::chrono::steady_clock::time_point cached_at{};
    if (!force_refresh && !cached_lpt.empty()) {
        auto age = std::chrono::steady_clock::now() - cached_at;
        if (age < std::chrono::minutes(5)) {
            return cached_lpt;
        }
    }

    std::ostringstream body;
    body << "client_id=" << urlEncode(MSAL_CLIENT_ID)
         << "&grant_type=refresh_token"
         << "&scope=" << urlEncode(XCLOUD_TRANSFER_SCOPE)
         << "&refresh_token=" << urlEncode(msal_refresh_token_);

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/x-www-form-urlencoded";
    headers["Cache-Control"] = "no-store, must-revalidate, no-cache";

    auto resp = http_.post(LIVE_TOKEN_URL, body.str(), headers);
    if (resp.status_code != 200) {
        last_error_ = resp.network_error
            ? "Network error while getting xCloud connect token."
            : ("xCloud connect token request failed. HTTP " +
               std::to_string(resp.status_code));
        lunar::diagnosticLog("auth",
                             "getXcloudTransferToken failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
        return "";
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) {
        last_error_ = "Failed to parse xCloud connect token response.";
        return "";
    }
    cJSON* at = cJSON_GetObjectItem(root, "access_token");
    std::string lpt = (at && cJSON_IsString(at) && at->valuestring) ? at->valuestring : "";
    cJSON_Delete(root);
    if (lpt.empty()) {
        last_error_ = "xCloud connect token response missing access_token/lpt.";
        return "";
    }

    cached_lpt = lpt;
    cached_at = std::chrono::steady_clock::now();
    lunar::diagnosticLog("auth", "getXcloudTransferToken ok len=%zu", lpt.size());
    last_error_.clear();
    return lpt;
}

bool AuthManager::loadTokens(const std::string& path) {
    tokens_.clear();
    clearInMemoryTokens();
    if (!tokens_.load(path)) return false;
    auto& d = tokens_.data();
    if (!d.access_token.empty()) msal_access_token_ = d.access_token;
    if (!d.refresh_token.empty()) msal_refresh_token_ = d.refresh_token;
    if (!d.gssv_token.empty()) gssv_token_ = d.gssv_token;
    if (!d.gssv_token.empty()) {
        home_token_.gs_token = d.gssv_token;
        home_token_.base_uri = d.gssv_base_uri;
        home_token_.duration_seconds = d.gssv_duration_seconds;
    }
    if (!d.gssv_cloud_token.empty()) {
        cloud_token_.gs_token = d.gssv_cloud_token;
        cloud_token_.base_uri = d.gssv_cloud_base_uri;
        cloud_token_.duration_seconds = d.gssv_cloud_duration_seconds;
    }
    if (!d.web_token.empty()) web_token_ = d.web_token;
    if (!d.gamertag.empty()) gamertag_ = d.gamertag;
    if (!d.user_hash.empty()) user_hash_ = d.user_hash;
    return hasSavedCredentials();
}

bool AuthManager::saveTokens(const std::string& path) {
    auto& d = tokens_.data();
    d.gssv_token = gssv_token_;
    d.gssv_base_uri = home_token_.base_uri;
    d.gssv_duration_seconds = home_token_.duration_seconds;
    d.gssv_cloud_token = cloud_token_.gs_token;
    d.gssv_cloud_base_uri = cloud_token_.base_uri;
    d.gssv_cloud_duration_seconds = cloud_token_.duration_seconds;
    d.web_token = web_token_;
    d.gamertag = gamertag_;
    d.user_hash = user_hash_;
    return tokens_.save(path);
}

void AuthManager::clearTokens() {
    tokens_.clear();
    clearInMemoryTokens();
    setState(AuthState::Idle);
}

void AuthManager::clearInMemoryTokens() {
    device_code_.clear();
    user_code_.clear();
    verification_uri_.clear();
    auth_message_.clear();
    msal_access_token_.clear();
    msal_refresh_token_.clear();
    web_token_.clear();
    gssv_token_.clear();
    home_token_ = {};
    cloud_token_ = {};
    xsts_gssv_token_.clear();
    user_hash_.clear();
    gamertag_.clear();
    last_error_.clear();
    last_refresh_attempt_ = {};
}

} // namespace lunar::auth
