#include "xbox_api_client.h"
#include "api_constants.h"
#include "../diagnostics.h"
#include <cJSON.h>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <set>
#include <cctype>
#include <cstring>

namespace lunar::api {
namespace {

#ifndef LUNARNX_XBOX_RESPONSE_TRACE
#define LUNARNX_XBOX_RESPONSE_TRACE 0
#endif

bool isHttpSuccess(int status_code) {
    return status_code >= 200 && status_code < 300;
}

#if LUNARNX_XBOX_RESPONSE_TRACE
const char* xboxResponseTracePath() {
#ifdef __SWITCH__
    return "sdmc:/switch/LunarNX/xbox_responses.jsonl";
#else
    return "/tmp/lunarnx_xbox_responses.jsonl";
#endif
}

void traceXboxResponse(const char* stage,
                       const std::string& url,
                       const HttpResponse& response) {
    lunar::ensureDiagnosticLogDirectory();
    static std::mutex trace_mutex;
    static bool initialized = false;
    static uint64_t sequence = 0;
    std::lock_guard<std::mutex> lock(trace_mutex);

    cJSON* entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "sequence", static_cast<double>(++sequence));
    cJSON_AddStringToObject(entry, "stage", stage ? stage : "unknown");
    cJSON_AddStringToObject(entry, "url", url.c_str());
    cJSON_AddNumberToObject(entry, "status", response.status_code);
    cJSON_AddBoolToObject(entry, "networkError", response.network_error);
    cJSON_AddStringToObject(entry, "error", response.error_message.c_str());
    cJSON_AddStringToObject(entry, "body", response.body.c_str());

    cJSON* headers = cJSON_CreateObject();
    for (const auto& [name, value] : response.headers) {
        cJSON_AddStringToObject(headers, name.c_str(), value.c_str());
    }
    cJSON_AddItemToObject(entry, "headers", headers);

    char* json = cJSON_PrintUnformatted(entry);
    cJSON_Delete(entry);
    if (!json) {
        return;
    }

    FILE* trace = std::fopen(xboxResponseTracePath(), initialized ? "a" : "w");
    if (trace) {
        std::fprintf(trace, "%s\n", json);
        std::fclose(trace);
        initialized = true;
    }
    std::free(json);
}
#else
void traceXboxResponse(const char*, const std::string&, const HttpResponse&) {}
#endif

std::string sessionIdFromPath(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= path.size()) {
        return "";
    }
    return path.substr(slash + 1);
}

void collectActiveSessionPaths(cJSON* node,
                               std::vector<std::string>& paths) {
    if (!node) {
        return;
    }
    if (cJSON_IsArray(node)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, node) {
            collectActiveSessionPaths(item, paths);
        }
        return;
    }
    if (!cJSON_IsObject(node)) {
        return;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, node) {
        if (item->string &&
            (std::strcmp(item->string, "sessionPath") == 0 ||
             std::strcmp(item->string, "path") == 0) &&
            cJSON_IsString(item) && item->valuestring) {
            paths.emplace_back(item->valuestring);
        }
        collectActiveSessionPaths(item, paths);
    }
}

std::string makeHomeDeviceInfoHeader(int width = 1920,
                                     int height = 1080,
                                     const char* os_name = "windows") {
    cJSON* dev_info = cJSON_CreateObject();
    cJSON* app_info = cJSON_CreateObject();
    cJSON* env = cJSON_CreateObject();
    cJSON_AddStringToObject(env, "clientAppId", "www.xbox.com");
    cJSON_AddStringToObject(env, "clientAppType", "browser");
    cJSON_AddStringToObject(env, "clientAppVersion", "29.9.35");
    cJSON_AddStringToObject(env, "clientSdkVersion", "10.6.8");
    cJSON_AddStringToObject(env, "httpEnvironment", "prod");
    cJSON_AddStringToObject(env, "sdkInstallId", "");
    cJSON_AddItemToObject(app_info, "env", env);
    cJSON_AddItemToObject(dev_info, "appInfo", app_info);

    cJSON* dev = cJSON_CreateObject();
    cJSON* hw = cJSON_CreateObject();
    cJSON_AddStringToObject(hw, "make", "Microsoft");
    cJSON_AddStringToObject(hw, "model", "unknown");
    cJSON_AddStringToObject(hw, "platformType", "desktop");
    cJSON_AddStringToObject(hw, "sdktype", "web");
    cJSON_AddItemToObject(dev, "hw", hw);

    cJSON* os = cJSON_CreateObject();
    cJSON_AddStringToObject(os, "name", os_name);
    cJSON_AddStringToObject(os, "ver", "22631.2715");
    cJSON_AddStringToObject(os, "platform", "desktop");
    cJSON_AddItemToObject(dev, "os", os);

    cJSON* display = cJSON_CreateObject();
    cJSON* dims = cJSON_CreateObject();
    cJSON_AddNumberToObject(dims, "widthInPixels", width);
    cJSON_AddNumberToObject(dims, "heightInPixels", height);
    cJSON_AddItemToObject(display, "dimensions", dims);

    cJSON* pixel = cJSON_CreateObject();
    cJSON_AddNumberToObject(pixel, "dpiX", 1);
    cJSON_AddNumberToObject(pixel, "dpiY", 1);
    cJSON_AddItemToObject(display, "pixelDensity", pixel);
    cJSON_AddItemToObject(dev, "displayInfo", display);

    cJSON* browser = cJSON_CreateObject();
    cJSON_AddStringToObject(browser, "browserName", "edge");
    cJSON_AddStringToObject(browser, "browserVersion", "140.0.3485.66");
    cJSON_AddItemToObject(dev, "browser", browser);

    cJSON_AddItemToObject(dev_info, "dev", dev);

    char* raw = cJSON_PrintUnformatted(dev_info);
    cJSON_Delete(dev_info);
    std::string result = raw ? raw : "";
    free(raw);
    return result;
}

std::string jsonString(cJSON* root, const char* key) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    return (item && cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

std::string serviceErrorMessage(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return "";
    }

    std::string code;
    std::string message;
    cJSON* details = cJSON_GetObjectItem(root, "errorDetails");
    if (details && cJSON_IsObject(details)) {
        code = jsonString(details, "code");
        message = jsonString(details, "message");
    }
    if (code.empty()) code = jsonString(root, "code");
    if (message.empty()) message = jsonString(root, "message");
    if (message.empty()) message = jsonString(root, "error");

    cJSON_Delete(root);

    if (!code.empty() && !message.empty()) return code + ": " + message;
    if (!message.empty()) return message;
    if (!code.empty()) return code;
    return "";
}

std::vector<XboxConsole> parseGssvConsoleList(const std::string& body, std::string* error) {
    std::vector<XboxConsole> consoles;
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        if (error) *error = "Xbox console list response could not be parsed.";
        lunar::diagnosticLog("xbox-api", "GSSV console list response parse failed");
        return consoles;
    }

    cJSON* results = cJSON_GetObjectItem(root, "results");
    if (results && cJSON_IsArray(results)) {
        int count = cJSON_GetArraySize(results);
        for (int i = 0; i < count; i++) {
            cJSON* dev = cJSON_GetArrayItem(results, i);
            if (!dev) continue;

            XboxConsole console;
            cJSON* id = cJSON_GetObjectItem(dev, "serverId");
            if (id && cJSON_IsString(id)) console.id = id->valuestring;

            cJSON* name = cJSON_GetObjectItem(dev, "deviceName");
            if (name && cJSON_IsString(name)) console.name = name->valuestring;

            cJSON* ct = cJSON_GetObjectItem(dev, "consoleType");
            if (ct && cJSON_IsString(ct)) console.console_type = ct->valuestring;

            cJSON* ps = cJSON_GetObjectItem(dev, "powerState");
            if (ps && cJSON_IsString(ps)) console.power_state = ps->valuestring;

            if (!console.id.empty()) {
                consoles.push_back(std::move(console));
            }
        }
    }

    cJSON_Delete(root);
    if (consoles.empty() && error) {
        *error = "No Xbox consoles found for this account.";
    }
    return consoles;
}

std::vector<XboxConsole> parseXccsConsoleList(const std::string& body, std::string* error) {
    std::vector<XboxConsole> consoles;
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        if (error) *error = "Xbox console list response could not be parsed.";
        lunar::diagnosticLog("xbox-api", "XCCS console list response parse failed");
        return consoles;
    }

    cJSON* result = cJSON_GetObjectItem(root, "result");
    if (result && cJSON_IsArray(result)) {
        int count = cJSON_GetArraySize(result);
        for (int i = 0; i < count; i++) {
            cJSON* dev = cJSON_GetArrayItem(result, i);
            if (!dev) continue;

            XboxConsole console;
            cJSON* id = cJSON_GetObjectItem(dev, "id");
            if (id && cJSON_IsString(id)) console.id = id->valuestring;

            cJSON* name = cJSON_GetObjectItem(dev, "name");
            if (name && cJSON_IsString(name)) console.name = name->valuestring;

            cJSON* ct = cJSON_GetObjectItem(dev, "consoleType");
            if (ct && cJSON_IsString(ct)) console.console_type = ct->valuestring;

            cJSON* ps = cJSON_GetObjectItem(dev, "powerState");
            if (ps && cJSON_IsString(ps)) console.power_state = ps->valuestring;

            cJSON* loc = cJSON_GetObjectItem(dev, "locale");
            if (loc && cJSON_IsString(loc)) console.locale = loc->valuestring;

            if (!console.id.empty()) {
                consoles.push_back(std::move(console));
            }
        }
    }

    cJSON_Delete(root);
    if (consoles.empty() && error) {
        *error = "No Xbox consoles found for this account.";
    }
    return consoles;
}

} // namespace

XboxApiClient::XboxApiClient(HttpClient& http, const std::string& web_token,
                               const std::string& user_hash, const std::string& gssv_token)
    : http_(http), web_token_(web_token), user_hash_(user_hash), gssv_token_(gssv_token),
      base_url_(constants::GSSV_HOME_BASE) {}

void XboxApiClient::setBaseUrl(const std::string& base_url) {
    base_url_ = base_url;
}

// =============================================================================
// Get Consoles
// =============================================================================
std::vector<XboxConsole> XboxApiClient::getConsoles(HttpClient::CancelCallback cancel) {
    last_error_.clear();

    std::string gssv_url = base_url_ + "/v6/servers/home?mr=50";
    std::map<std::string, std::string> gssv_headers;
    gssv_headers["Content-Type"] = "application/json";
    gssv_headers["X-MS-Device-Info"] = makeHomeDeviceInfoHeader();
    gssv_headers["Authorization"] = "Bearer " + gssv_token_;

    auto gssv_resp = http_.get(gssv_url, gssv_headers, cancel);
    traceXboxResponse("get-consoles", gssv_url, gssv_resp);
    if (gssv_resp.status_code == 200) {
        auto consoles = parseGssvConsoleList(gssv_resp.body, &last_error_);
        if (!consoles.empty()) {
            lunar::diagnosticLog("xbox-api", "GSSV console list succeeded count=%zu",
                                 consoles.size());
            return consoles;
        }
        lunar::diagnosticLog("xbox-api", "GSSV console list returned empty");
        return consoles;
    }

    last_error_ = gssv_resp.network_error
        ? "Could not reach Xbox services. Check WiFi and try again."
        : "Xbox console list request failed. HTTP " + std::to_string(gssv_resp.status_code) + ".";
    lunar::diagnosticLog("xbox-api", "GSSV console list failed status=%d network=%s error=%s",
                         gssv_resp.status_code,
                         gssv_resp.network_error ? "true" : "false",
                         gssv_resp.error_message.c_str());

#if defined(__SWITCH__)
    return {};
#else
    std::vector<XboxConsole> consoles;
    std::string url = "https://xccs.xboxlive.com/lists/devices?queryCurrentDevice=false&includeStorageDevices=true";
    std::string auth_header = "XBL3.0 x=" + user_hash_ + ";" + web_token_;

    std::map<std::string, std::string> headers;
    headers["Authorization"] = auth_header;
    headers["Accept-Language"] = "en-US";
    headers["x-xbl-contract-version"] = "2";
    headers["x-xbl-client-name"] = "XboxApp";
    headers["x-xbl-client-type"] = "UWA";
    headers["x-xbl-client-version"] = "39.39.22001.0";

    auto resp = http_.get(url, headers, cancel);
    traceXboxResponse("get-consoles", url, resp);

    if (resp.status_code != 200) {
        last_error_ = resp.network_error
            ? "Could not reach Xbox services. Check WiFi and try again."
            : "Xbox console list request failed. HTTP " + std::to_string(resp.status_code) + ".";
        fprintf(stderr, "[api] Get consoles failed: HTTP %d\n", resp.status_code);
        lunar::diagnosticLog("xbox-api", "Get consoles failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
        return consoles;
    }

    consoles = parseXccsConsoleList(resp.body, &last_error_);
    if (consoles.empty()) {
        lunar::diagnosticLog("xbox-api", "Get consoles returned an empty list");
    }
    return consoles;
#endif
}

// =============================================================================
// Create Session
// =============================================================================
std::string XboxApiClient::sessionBasePath() const {
    return base_url_ + "/v5/sessions/" + gssvSessionKindPath(session_kind_);
}

std::string XboxApiClient::createSession(const std::string& server_id, int width, int height,
                                         HttpClient::CancelCallback cancel) {
    CreateSessionRequest request;
    request.kind = GssvSessionKind::Home;
    request.server_id = server_id;
    request.width = width;
    request.height = height;
    return createSession(request, cancel);
}

std::string XboxApiClient::createSession(const CreateSessionRequest& request,
                                         HttpClient::CancelCallback cancel) {
    last_error_.clear();
    session_kind_ = request.kind;
    std::string url = sessionBasePath() + "/play";
    const char* os_name = !request.os_name.empty()
        ? request.os_name.c_str()
        : (request.height >= 1080 ? "windows" : "android");
    const std::string device_info = makeHomeDeviceInfoHeader(request.width, request.height, os_name);

    cJSON* settings = cJSON_CreateObject();
    cJSON_AddStringToObject(settings, "nanoVersion", "V3;WebrtcTransport.dll");
    cJSON_AddBoolToObject(settings, "enableTextToSpeech", false);
    cJSON_AddNumberToObject(settings, "highContrast", 0);
    cJSON_AddStringToObject(settings, "locale",
                            request.locale.empty() ? "en-US"
                                                   : request.locale.c_str());
    // XStreaming/Greenlight use the GSSV SDP/ICE exchange path.
    cJSON_AddBoolToObject(settings, "useIceConnection", false);
    cJSON_AddNumberToObject(settings, "timezoneOffsetMinutes", 120);
    cJSON_AddStringToObject(settings, "sdkType", "web");
    cJSON_AddStringToObject(settings, "osName", os_name);

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "clientSessionId", "");
    if (request.kind == GssvSessionKind::Cloud) {
        cJSON_AddStringToObject(body, "titleId", request.title_id.c_str());
        cJSON_AddStringToObject(body, "serverId", "");
    } else {
        cJSON_AddStringToObject(body, "titleId", "");
        cJSON_AddStringToObject(body, "serverId", request.server_id.c_str());
    }
    cJSON_AddStringToObject(body, "systemUpdateGroup", "");
    cJSON_AddItemToObject(body, "settings", settings);

    cJSON* fallback = cJSON_CreateArray();
    cJSON_AddItemToObject(body, "fallbackRegionNames", fallback);

    char* body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    std::map<std::string, std::string> headers;
    headers["Accept"] = "application/json";
    headers["Content-Type"] = "application/json";
    headers["X-MS-Device-Info"] = device_info;
    headers["Authorization"] = "Bearer " + gssv_token_;

    auto resp = http_.post(url, body_str, headers, cancel);
    traceXboxResponse("create-session", url, resp);
    free(body_str);

    if (!isHttpSuccess(resp.status_code)) {
        if (resp.network_error) {
            last_error_ = "Could not create Xbox session. Check WiFi and try again.";
        } else {
            std::string detail = serviceErrorMessage(resp.body);
            last_error_ = "Xbox session creation failed. HTTP " +
                          std::to_string(resp.status_code);
            if (!detail.empty()) last_error_ += ": " + detail;
        }
        fprintf(stderr, "[api] Create session failed: HTTP %d\n", resp.status_code);
        lunar::diagnosticLog("xbox-api", "Create session failed kind=%s status=%d network=%s error=%s",
                             gssvSessionKindPath(request.kind),
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
        return "";
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) {
        last_error_ = "Xbox session creation response could not be parsed.";
        lunar::diagnosticLog("xbox-api", "Create session response parse failed");
        return "";
    }

    std::string session_id;
    cJSON* sid = cJSON_GetObjectItem(root, "sessionId");
    if (sid && cJSON_IsString(sid)) session_id = sid->valuestring;
    if (session_id.empty()) {
        cJSON* path = cJSON_GetObjectItem(root, "sessionPath");
        if (path && cJSON_IsString(path) && path->valuestring) {
            session_id = sessionIdFromPath(path->valuestring);
        }
    }

    cJSON_Delete(root);
    session_id_ = session_id;
    if (session_id.empty()) {
        last_error_ = "Xbox session creation response did not include a session ID.";
        lunar::diagnosticLog("xbox-api", "Create session response missing sessionId");
    }
    return session_id;
}

// =============================================================================
// Poll Session State
// =============================================================================
std::string XboxApiClient::pollSessionState(const std::string& session_id,
                                            HttpClient::CancelCallback cancel) {
    std::string url = sessionBasePath() + "/" + session_id + "/state";

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer " + gssv_token_;

    auto resp = http_.get(url, headers, cancel);
    traceXboxResponse("session-state", url, resp);

    if (resp.status_code != 200) {
        if (resp.network_error) {
            last_error_ = "Could not poll Xbox session state. Check WiFi and try again.";
        } else {
            const std::string detail = serviceErrorMessage(resp.body);
            last_error_ = "Xbox session state request failed. HTTP " +
                          std::to_string(resp.status_code) + ".";
            if (!detail.empty()) {
                last_error_ += " " + detail;
            }
        }
        lunar::diagnosticLog("xbox-api", "Poll session state failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
        return "Error";
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) {
        last_error_ = "Xbox session state response could not be parsed.";
        lunar::diagnosticLog("xbox-api", "Poll session state response parse failed");
        return "Error";
    }

    std::string state;
    cJSON* st = cJSON_GetObjectItem(root, "state");
    if (st && cJSON_IsString(st)) state = st->valuestring;

    if (state == "Failed" || state == "Error") {
        last_error_ = serviceErrorMessage(resp.body);
        if (last_error_.empty()) {
            last_error_ = "Xbox session state failed.";
        }
        lunar::diagnosticLog("xbox-api", "Poll session state returned %s detail=%s",
                             state.c_str(), last_error_.c_str());
    }

    cJSON_Delete(root);
    return state;
}

// =============================================================================
// Get Session Configuration
// =============================================================================
SessionConfig XboxApiClient::getSessionConfig(const std::string& session_id,
                                              HttpClient::CancelCallback cancel) {
    SessionConfig config = {};
    std::string url = sessionBasePath() + "/" + session_id + "/configuration";

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer " + gssv_token_;

    auto resp = http_.get(url, headers, cancel);
    traceXboxResponse("session-configuration", url, resp);

    if (resp.status_code != 200) {
        last_error_ = resp.network_error
            ? "Could not get Xbox session config. Check WiFi and try again."
            : "Xbox session config request failed. HTTP " + std::to_string(resp.status_code) + ".";
        lunar::diagnosticLog("xbox-api", "Get session config failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
        return config;
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) {
        last_error_ = "Xbox session config response could not be parsed.";
        lunar::diagnosticLog("xbox-api", "Get session config response parse failed");
        return config;
    }

    cJSON* sd = cJSON_GetObjectItem(root, "serverDetails");
    if (sd) {
        cJSON* ip = cJSON_GetObjectItem(sd, "ipV4Address");
        if (!ip) ip = cJSON_GetObjectItem(sd, "ipAddress");
        if (ip && cJSON_IsString(ip)) config.ip_address = ip->valuestring;

        cJSON* port = cJSON_GetObjectItem(sd, "ipV4Port");
        if (!port) port = cJSON_GetObjectItem(sd, "port");
        if (port && cJSON_IsNumber(port)) config.port = static_cast<uint16_t>(port->valueint);

        cJSON* ice_path = cJSON_GetObjectItem(sd, "iceExchangePath");
        if (ice_path && cJSON_IsString(ice_path)) config.ice_exchange_path = ice_path->valuestring;

        cJSON* srtp = cJSON_GetObjectItem(sd, "srtp");
        if (srtp) {
            cJSON* key = cJSON_GetObjectItem(srtp, "key");
            if (key && cJSON_IsString(key)) config.srtp_key = key->valuestring;
        }
    }

    cJSON* keep_alive = cJSON_GetObjectItem(root, "keepAlivePulseInSeconds");
    if (keep_alive && cJSON_IsNumber(keep_alive)) config.keep_alive_seconds = keep_alive->valueint;

    cJSON_Delete(root);
    return config;
}

// =============================================================================
// SDP Exchange
// =============================================================================
bool XboxApiClient::sendSdpOffer(const std::string& session_id, const std::string& sdp,
                                 HttpClient::CancelCallback cancel) {
    std::string url = sessionBasePath() + "/" + session_id + "/sdp";

    cJSON* chat_config = cJSON_CreateObject();
    cJSON_AddNumberToObject(chat_config, "bytesPerSample", 2);
    cJSON_AddNumberToObject(chat_config, "expectedClipDurationMs", 20);
    cJSON* format = cJSON_CreateObject();
    cJSON_AddStringToObject(format, "codec", "opus");
    cJSON_AddStringToObject(format, "container", "webm");
    cJSON_AddItemToObject(chat_config, "format", format);
    cJSON_AddNumberToObject(chat_config, "numChannels", 1);
    cJSON_AddNumberToObject(chat_config, "sampleFrequencyHz", 24000);

    cJSON* chat = cJSON_CreateObject();
    cJSON_AddNumberToObject(chat, "minVersion", 1);
    cJSON_AddNumberToObject(chat, "maxVersion", 1);

    cJSON* control = cJSON_CreateObject();
    cJSON_AddNumberToObject(control, "minVersion", 1);
    cJSON_AddNumberToObject(control, "maxVersion", 3);

    cJSON* input = cJSON_CreateObject();
    cJSON_AddNumberToObject(input, "minVersion", 1);
    cJSON_AddNumberToObject(input, "maxVersion", 8);

    cJSON* message = cJSON_CreateObject();
    cJSON_AddNumberToObject(message, "minVersion", 1);
    cJSON_AddNumberToObject(message, "maxVersion", 1);

    cJSON* config = cJSON_CreateObject();
    cJSON_AddItemToObject(config, "chatConfiguration", chat_config);
    cJSON_AddItemToObject(config, "chat", chat);
    cJSON_AddItemToObject(config, "control", control);
    cJSON_AddItemToObject(config, "input", input);
    cJSON_AddItemToObject(config, "message", message);

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "messageType", "offer");
    cJSON_AddStringToObject(body, "sdp", sdp.c_str());
    cJSON_AddItemToObject(body, "configuration", config);

    char* body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    std::map<std::string, std::string> headers;
    headers["Accept"] = "application/json";
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer " + gssv_token_;

    auto resp = http_.post(url, body_str, headers, cancel);
    traceXboxResponse("post-sdp", url, resp);
    free(body_str);

    if (!isHttpSuccess(resp.status_code)) {
        std::string service_error = serviceErrorMessage(resp.body);
        last_error_ = resp.network_error
            ? "Could not send WebRTC offer. Check WiFi and try again."
            : "Sending WebRTC offer failed. HTTP " + std::to_string(resp.status_code) + "." +
                  (service_error.empty() ? "" : " " + service_error);
        fprintf(stderr, "[api] Send SDP failed: HTTP %d\n", resp.status_code);
        lunar::diagnosticLog("xbox-api", "Send SDP failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
        return false;
    }
    return true;
}

std::string XboxApiClient::getSdpAnswer(const std::string& session_id,
                                        HttpClient::CancelCallback cancel) {
    std::string url = sessionBasePath() + "/" + session_id + "/sdp";

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer " + gssv_token_;

    auto resp = http_.get(url, headers, cancel);
    traceXboxResponse("get-sdp", url, resp);

    if (resp.status_code == 204) {
        return "";
    }

    if (resp.status_code != 200) {
        last_error_ = resp.network_error
            ? "Could not get WebRTC answer. Check WiFi and try again."
            : "Getting WebRTC answer failed. HTTP " + std::to_string(resp.status_code) + ".";
        fprintf(stderr, "[api] Get SDP failed: HTTP %d\n", resp.status_code);
        lunar::diagnosticLog("xbox-api", "Get SDP failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
        return "";
    }

    // Outer JSON: {"exchangeResponse": "{\"sdp\":...,\"sdpType\":\"answer\",...}"}
    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) {
        last_error_ = "WebRTC answer response could not be parsed.";
        lunar::diagnosticLog("xbox-api", "Get SDP response parse failed");
        return "";
    }

    std::string exchange;
    cJSON* exr = cJSON_GetObjectItem(root, "exchangeResponse");
    if (exr && cJSON_IsString(exr)) exchange = exr->valuestring;
    cJSON_Delete(root);

    if (exchange.empty()) {
        last_error_ = "WebRTC answer response was empty.";
        lunar::diagnosticLog("xbox-api", "Get SDP response missing exchangeResponse");
        return "";
    }

    // Inner JSON: extract actual SDP
    cJSON* inner = cJSON_Parse(exchange.c_str());
    if (!inner) return exchange;  // Fallback: return raw

    std::string sdp;
    cJSON* sdp_item = cJSON_GetObjectItem(inner, "sdp");
    if (sdp_item && cJSON_IsString(sdp_item)) sdp = sdp_item->valuestring;
    cJSON_Delete(inner);

    return sdp.empty() ? exchange : sdp;
}

// =============================================================================
// ICE Exchange
// =============================================================================
bool XboxApiClient::sendIceCandidates(const std::string& session_id,
                                       const std::string& ice_json,
                                       HttpClient::CancelCallback cancel) {
    std::string url = sessionBasePath() + "/" + session_id + "/ice";

    std::map<std::string, std::string> headers;
    headers["Accept"] = "application/json";
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer " + gssv_token_;

    auto resp = http_.post(url, ice_json, headers, cancel);
    traceXboxResponse("post-ice", url, resp);

    const bool ok = isHttpSuccess(resp.status_code);
    if (!ok) {
        last_error_ = resp.network_error
            ? "Could not send ICE candidates. Check WiFi and try again."
            : "Sending ICE candidates failed. HTTP " + std::to_string(resp.status_code) + ".";
        lunar::diagnosticLog("xbox-api", "Send ICE failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
    }
    return ok;
}

std::string XboxApiClient::getIceCandidates(const std::string& session_id,
                                            HttpClient::CancelCallback cancel) {
    std::string url = sessionBasePath() + "/" + session_id + "/ice";

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer " + gssv_token_;

    auto resp = http_.get(url, headers, cancel);
    traceXboxResponse("get-ice", url, resp);

    if (resp.status_code == 204) {
        return "";
    }

    if (resp.status_code != 200) {
        last_error_ = resp.network_error
            ? "Could not get ICE candidates. Check WiFi and try again."
            : "Getting ICE candidates failed. HTTP " + std::to_string(resp.status_code) + ".";
        lunar::diagnosticLog("xbox-api", "Get ICE failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
        return "";
    }

    // Outer JSON: {"exchangeResponse": "[{\"candidate\":...,\"sdpMid\":...,...}]"}
    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) {
        last_error_ = "ICE candidate response could not be parsed.";
        lunar::diagnosticLog("xbox-api", "Get ICE response parse failed");
        return "";
    }

    std::string exchange;
    cJSON* exr = cJSON_GetObjectItem(root, "exchangeResponse");
    if (exr && cJSON_IsString(exr)) exchange = exr->valuestring;
    cJSON_Delete(root);

    return exchange;
}

// =============================================================================
// Delete Session
// =============================================================================
KeepAliveResult XboxApiClient::sendKeepAliveDetailed(
    const std::string& session_id, HttpClient::CancelCallback cancel) {
    KeepAliveResult result;
    std::string url = sessionBasePath() + "/" + session_id + "/keepalive";

    std::map<std::string, std::string> headers;
    headers["Accept"] = "application/json";
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer " + gssv_token_;

    auto resp = http_.post(url, "{}", headers, cancel);
    traceXboxResponse("keepalive", url, resp);
    result.ok = isHttpSuccess(resp.status_code);
    result.status_code = resp.status_code;
    result.network_error = resp.network_error;
    if (!result.ok) {
        last_error_ = resp.network_error
            ? "Could not send Xbox session keepalive. Check WiFi and try again."
            : "Xbox session keepalive failed. HTTP " + std::to_string(resp.status_code) + ".";
        lunar::diagnosticLog("xbox-api", "Keepalive failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
    }
    return result;
}

bool XboxApiClient::sendKeepAlive(const std::string& session_id,
                                  HttpClient::CancelCallback cancel) {
    return sendKeepAliveDetailed(session_id, cancel).ok;
}

bool XboxApiClient::deleteSession(const std::string& session_id, HttpClient::CancelCallback cancel) {
    std::string url = sessionBasePath() + "/" + session_id;

    std::map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + gssv_token_;

    auto resp = http_.del(url, headers, cancel);
    traceXboxResponse("delete-session", url, resp);
    const bool ok = isHttpSuccess(resp.status_code);
    if (!ok) {
        last_error_ = resp.network_error
            ? "Could not delete Xbox session. Check WiFi and try again."
            : "Deleting Xbox session failed. HTTP " + std::to_string(resp.status_code) + ".";
        lunar::diagnosticLog("xbox-api", "Delete session failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
    }
    return ok;
}

bool XboxApiClient::cleanupActiveSessions(GssvSessionKind kind,
                                          HttpClient::CancelCallback cancel) {
    const std::string path = "/v5/sessions/" +
                             std::string(gssvSessionKindPath(kind)) + "/active";
    const std::string url = base_url_ + path;

    std::map<std::string, std::string> headers;
    headers["Accept"] = "application/json";
    headers["Content-Type"] = "application/json";
    headers["X-MS-Device-Info"] = kind == GssvSessionKind::Home
        ? makeHomeDeviceInfoHeader(1280, 720, "android")
        : makeHomeDeviceInfoHeader(1920, 1080, "tizen");
    headers["Authorization"] = "Bearer " + gssv_token_;

    auto response = http_.get(url, headers, cancel);
    traceXboxResponse("active-sessions", url, response);
    if (response.status_code == 404 || response.status_code == 204) {
        return true;
    }
    if (!isHttpSuccess(response.status_code)) {
        lunar::diagnosticLog("xbox-api",
                             "Active session cleanup lookup failed kind=%s status=%d network=%s error=%s",
                             gssvSessionKindPath(kind),
                             response.status_code,
                             response.network_error ? "true" : "false",
                             response.error_message.c_str());
        return false;
    }

    cJSON* root = cJSON_Parse(response.body.c_str());
    if (!root) {
        lunar::diagnosticLog("xbox-api", "Active session cleanup response parse failed");
        return false;
    }
    std::vector<std::string> paths;
    collectActiveSessionPaths(root, paths);
    cJSON_Delete(root);

    std::set<std::string> unique_paths;
    const std::string expected_prefix = "/v5/sessions/" +
                                        std::string(gssvSessionKindPath(kind)) + "/";
    for (const auto& raw_path : paths) {
        if (raw_path.empty()) {
            continue;
        }
        std::string session_path = raw_path;
        if (session_path.rfind("http://", 0) == 0 ||
            session_path.rfind("https://", 0) == 0) {
            const auto scheme_end = session_path.find('/', session_path.find("://") + 3);
            session_path = scheme_end == std::string::npos
                ? "/"
                : session_path.substr(scheme_end);
        }
        if (session_path.front() != '/') {
            session_path.insert(session_path.begin(), '/');
        }
        if (session_path.rfind(expected_prefix, 0) != 0 ||
            session_path == expected_prefix + "active") {
            lunar::diagnosticLog("xbox-api",
                                 "Ignoring unexpected active-session path=%s",
                                 session_path.c_str());
            continue;
        }
        if (!unique_paths.insert(session_path).second) {
            continue;
        }

        const std::string delete_url = base_url_ + session_path;
        auto deleted = http_.del(delete_url, headers, cancel);
        traceXboxResponse("active-session-delete", delete_url, deleted);
        if (deleted.status_code == 404 || isHttpSuccess(deleted.status_code)) {
            lunar::diagnosticLog("xbox-api", "Removed stale session path=%s",
                                 session_path.c_str());
            continue;
        }
        lunar::diagnosticLog("xbox-api",
                             "Stale session delete failed path=%s status=%d network=%s error=%s",
                             session_path.c_str(),
                             deleted.status_code,
                             deleted.network_error ? "true" : "false",
                             deleted.error_message.c_str());
    }
    return true;
}

// =============================================================================
// Cloud ReadyToConnect
// =============================================================================
bool XboxApiClient::sendConnect(const std::string& session_id,
                                const std::string& user_token,
                                HttpClient::CancelCallback cancel) {
    last_error_.clear();
    if (user_token.empty()) {
        last_error_ = "Cloud connect requires an MSAL user token.";
        return false;
    }

    std::string url = sessionBasePath() + "/" + session_id + "/connect";
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "userToken", user_token.c_str());
    char* body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    std::map<std::string, std::string> headers;
    headers["Accept"] = "application/json";
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer " + gssv_token_;

    auto resp = http_.post(url, body_str ? body_str : "{}", headers, cancel);
    free(body_str);
    traceXboxResponse("session-connect", url, resp);

    if (!isHttpSuccess(resp.status_code)) {
        if (resp.network_error) {
            last_error_ = "Could not complete cloud session connect. Check WiFi and try again.";
        } else {
            std::string detail = serviceErrorMessage(resp.body);
            last_error_ = "Cloud session connect failed. HTTP " +
                          std::to_string(resp.status_code);
            if (!detail.empty()) last_error_ += ": " + detail;
        }
        lunar::diagnosticLog("xbox-api", "Send connect failed status=%d network=%s error=%s",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str());
        return false;
    }
    return true;
}

// =============================================================================
// Cloud title helpers
// =============================================================================
std::string XboxApiClient::normalizeProductId(std::string id) {
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return id;
}

static std::string salvageCloudTitleJson(std::string body) {
    if (body.empty()) return body;
    if (cJSON* t = cJSON_Parse(body.c_str())) {
        cJSON_Delete(t);
        return body;
    }
    const auto pos = body.rfind("},{");
    if (pos == std::string::npos) return body;
    std::string cut = body.substr(0, pos + 1);
    int braces = 0, brackets = 0;
    for (char c : cut) {
        if (c == '{') ++braces;
        else if (c == '}') --braces;
        else if (c == '[') ++brackets;
        else if (c == ']') --brackets;
    }
    while (brackets > 0) { cut.push_back(']'); --brackets; }
    while (braces > 0) { cut.push_back('}'); --braces; }
    return cut;
}

std::vector<CloudTitle> XboxApiClient::parseCloudTitleList(const std::string& body) {
    std::vector<CloudTitle> titles;
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return titles;

    cJSON* results = cJSON_GetObjectItem(root, "results");
    if (!results || !cJSON_IsArray(results)) {
        // Some payloads are bare arrays.
        if (cJSON_IsArray(root)) results = root;
    }
    if (results && cJSON_IsArray(results)) {
        const int count = cJSON_GetArraySize(results);
        for (int i = 0; i < count; ++i) {
            cJSON* item = cJSON_GetArrayItem(results, i);
            if (!item) continue;
            CloudTitle title;
            title.title_id = jsonString(item, "titleId");
            if (title.title_id.empty()) title.title_id = jsonString(item, "TitleId");
            cJSON* details = cJSON_GetObjectItem(item, "details");
            if (details && cJSON_IsObject(details)) {
                title.product_id = jsonString(details, "productId");
                if (title.product_id.empty()) title.product_id = jsonString(details, "ProductId");
            }
            if (title.product_id.empty()) title.product_id = jsonString(item, "productId");
            if (!title.product_id.empty()) title.product_id = normalizeProductId(title.product_id);
            title.name = jsonString(item, "name");
            if (title.name.empty()) title.name = jsonString(item, "ProductTitle");
            if (title.title_id.empty() && title.product_id.empty()) continue;
            if (title.name.empty()) title.name = !title.title_id.empty() ? title.title_id : title.product_id;
            titles.push_back(std::move(title));
        }
    }
    cJSON_Delete(root);
    return titles;
}

std::vector<std::string> XboxApiClient::loadOfficialProductIds() const {
    std::vector<std::string> ids;
    // Prefer romfs/local packaged titles.json when available.
    const char* candidates[] = {
#ifdef __SWITCH__
        "romfs:/xcloud_titles.json",
        "sdmc:/switch/LunarNX/xcloud_titles.json",
#else
        "res/xcloud_titles.json",
        "xcloud_titles.json",
#endif
    };
    for (const char* path : candidates) {
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); continue; }
        long len = std::ftell(f);
        if (len <= 0) { std::fclose(f); continue; }
        std::rewind(f);
        std::string buf(static_cast<size_t>(len), '\0');
        if (std::fread(buf.data(), 1, static_cast<size_t>(len), f) != static_cast<size_t>(len)) {
            std::fclose(f);
            continue;
        }
        std::fclose(f);
        cJSON* root = cJSON_Parse(buf.c_str());
        if (!root) continue;
        cJSON* products = cJSON_GetObjectItem(root, "Products");
        if (products && cJSON_IsArray(products)) {
            const int count = cJSON_GetArraySize(products);
            for (int i = 0; i < count; ++i) {
                cJSON* item = cJSON_GetArrayItem(products, i);
                if (item && cJSON_IsString(item) && item->valuestring) {
                    ids.push_back(normalizeProductId(item->valuestring));
                }
            }
        }
        cJSON_Delete(root);
        if (!ids.empty()) {
            lunar::diagnosticLog("xbox-api", "Loaded official titles list from %s bytes=%ld", path, len);
            break;
        }
    }
    return ids;
}

std::vector<CloudTitle> XboxApiClient::hydrateCatalogProducts(
    const std::vector<std::string>& product_ids,
    const std::map<std::string, CloudTitle>& v2_by_product,
    HttpClient::CancelCallback cancel) {
    std::vector<CloudTitle> out;
    if (product_ids.empty()) return out;

    constexpr size_t kChunk = 100;
    for (size_t offset = 0; offset < product_ids.size(); offset += kChunk) {
        if (cancel && cancel()) return out;
        const size_t end = std::min(offset + kChunk, product_ids.size());
        cJSON* body = cJSON_CreateObject();
        cJSON* products = cJSON_CreateArray();
        for (size_t i = offset; i < end; ++i) {
            cJSON_AddItemToArray(products, cJSON_CreateString(product_ids[i].c_str()));
        }
        cJSON_AddItemToObject(body, "Products", products);
        char* body_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);

        std::map<std::string, std::string> headers;
        headers["Accept"] = "application/json";
        headers["Content-Type"] = "application/json";
        headers["Accept-Language"] = catalog_language_;
        headers["ms-cv"] = "0";
        headers["calling-app-name"] = "Xbox Cloud Gaming Web";
        headers["calling-app-version"] = "24.17.63";

        const std::string catalog_origin = catalog_base_url_.empty()
            ? "https://catalog.gamepass.com"
            : catalog_base_url_;
        const std::string url = catalog_origin +
            "/v3/products?market=US&language=" + catalog_language_ +
            "&hydration=RemoteLowJade0";
        auto resp = http_.post(url, body_str ? body_str : "{}", headers, cancel);
        free(body_str);
        traceXboxResponse("catalog-products", url, resp);

        if (resp.status_code != 200 && resp.body.empty()) {
            lunar::diagnosticLog("xbox-api", "Catalog hydration chunk failed offset=%zu status=%d",
                                 offset, resp.status_code);
            continue;
        }
        if (resp.status_code != 200) {
            lunar::diagnosticLog("xbox-api",
                                 "Catalog hydration chunk soft-fail offset=%zu status=%d body_len=%zu err=%s",
                                 offset, resp.status_code, resp.body.size(),
                                 resp.error_message.c_str());
        }

        cJSON* root = cJSON_Parse(resp.body.c_str());
        if (!root) {
            std::string salvaged = salvageCloudTitleJson(resp.body);
            root = cJSON_Parse(salvaged.c_str());
            if (!root) continue;
        }
        cJSON* products_obj = cJSON_GetObjectItem(root, "Products");
        if (products_obj && cJSON_IsObject(products_obj)) {
            for (cJSON* child = products_obj->child; child; child = child->next) {
                if (!child->string) continue;
                CloudTitle title;
                title.product_id = normalizeProductId(child->string);
                title.name = jsonString(child, "ProductTitle");
                title.publisher = jsonString(child, "PublisherName");
                title.title_id = jsonString(child, "titleId");
                if (title.title_id.empty()) title.title_id = jsonString(child, "XCloudTitleId");

                cJSON* poster = cJSON_GetObjectItem(child, "Image_Poster");
                if (!poster || !cJSON_IsObject(poster)) {
                    poster = cJSON_GetObjectItem(child, "Image_Tile");
                }
                if (poster && cJSON_IsObject(poster)) {
                    std::string url_field = jsonString(poster, "URL");
                    if (url_field.empty()) url_field = jsonString(poster, "url");
                    if (!url_field.empty()) {
                        if (url_field.rfind("//", 0) == 0) url_field = "https:" + url_field;
                        title.image_url = url_field;
                    }
                }

                auto it = v2_by_product.find(title.product_id);
                if (it != v2_by_product.end()) {
                    if (title.title_id.empty()) title.title_id = it->second.title_id;
                    if (title.name.empty()) title.name = it->second.name;
                    if (title.image_url.empty()) title.image_url = it->second.image_url;
                }
                if (title.title_id.empty()) continue;
                if (title.name.empty()) title.name = title.title_id;
                out.push_back(std::move(title));
            }
        }
        cJSON_Delete(root);
    }

    std::sort(out.begin(), out.end(), [](const CloudTitle& a, const CloudTitle& b) {
        return a.name < b.name;
    });
    // Dedupe by ProductTitle
    std::vector<CloudTitle> deduped;
    deduped.reserve(out.size());
    for (auto& title : out) {
        bool exists = false;
        for (const auto& kept : deduped) {
            if (kept.name == title.name) { exists = true; break; }
        }
        if (!exists) deduped.push_back(std::move(title));
    }
    return deduped;
}

std::vector<CloudTitle> XboxApiClient::getRecentCloudTitles(HttpClient::CancelCallback cancel) {
    last_error_.clear();
    std::string url = base_url_ + "/v2/titles/mru?mr=25";
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer " + gssv_token_;
    headers["Accept-Language"] = catalog_language_;

    auto resp = http_.get(url, headers, cancel);
    traceXboxResponse("cloud-titles-mru", url, resp);
    if (resp.status_code != 200) {
        last_error_ = resp.network_error
            ? "Could not load recent cloud titles. Check WiFi and try again."
            : "Recent cloud titles request failed. HTTP " + std::to_string(resp.status_code) + ".";
        lunar::diagnosticLog("xbox-api", "Recent cloud titles failed status=%d", resp.status_code);
        return {};
    }
    auto titles = parseCloudTitleList(resp.body);
    for (auto& t : titles) t.is_recent = true;
    if (titles.empty() && last_error_.empty()) {
        last_error_ = "No recent cloud titles found for this account.";
    }
    return titles;
}

std::vector<CloudTitle> XboxApiClient::getCloudTitles(HttpClient::CancelCallback cancel) {
    last_error_.clear();
    std::string url = base_url_ + "/v2/titles";
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer " + gssv_token_;
    headers["Accept-Language"] = catalog_language_;

    auto resp = http_.get(url, headers, cancel);
    traceXboxResponse("cloud-titles", url, resp);

    std::string body = salvageCloudTitleJson(std::move(resp.body));
    auto titles = parseCloudTitleList(body);
    if (!titles.empty()) {
        if (resp.network_error) {
            lunar::diagnosticLog(
                "xbox-api",
                "Cloud titles partial body accepted count=%zu body_len=%zu status=%d err=%s",
                titles.size(), body.size(), resp.status_code,
                resp.error_message.c_str());
            last_error_.clear();
        }
        return titles;
    }

    if (resp.status_code != 200) {
        last_error_ = resp.network_error
            ? ("Could not load full cloud library (" +
               (resp.error_message.empty() ? "network/timeout" : resp.error_message) +
               "). Try again or use a more stable network.")
            : "Cloud titles request failed. HTTP " + std::to_string(resp.status_code) + ".";
        lunar::diagnosticLog("xbox-api", "Cloud titles failed status=%d body_len=%zu",
                             resp.status_code, body.size());
        return {};
    }
    if (last_error_.empty()) {
        last_error_ = "No cloud titles found for this account.";
    }
    return titles;
}

std::vector<std::string> XboxApiClient::getNewCloudProductIds(HttpClient::CancelCallback cancel) {
    std::vector<std::string> ids;
    last_error_.clear();
    const std::string catalog_origin = catalog_base_url_.empty()
        ? "https://catalog.gamepass.com"
        : catalog_base_url_;
    const std::string sigls_url = catalog_origin +
        "/sigls/v2?id=f13cf6b4-57e6-4459-89df-6aec18cf0538" +
        "&market=US&language=" + catalog_language_;
    auto resp = http_.get(sigls_url,
                          {{"Accept", "application/json"},
                           {"Accept-Language", catalog_language_}},
                          cancel);
    traceXboxResponse("cloud-new-titles", sigls_url, resp);
    if (resp.status_code != 200) {
        lunar::diagnosticLog("xbox-api", "New titles sigls failed status=%d", resp.status_code);
        return ids;
    }
    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return ids;
    cJSON* arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "siglAttributes");
    if (arr && cJSON_IsArray(arr)) {
        const int count = cJSON_GetArraySize(arr);
        for (int i = 0; i < count; ++i) {
            cJSON* item = cJSON_GetArrayItem(arr, i);
            if (!item) continue;
            std::string id = jsonString(item, "id");
            if (id.empty()) id = jsonString(item, "productId");
            if (id.empty() && cJSON_IsString(item) && item->valuestring) id = item->valuestring;
            if (id.size() < 6) continue;
            ids.push_back(normalizeProductId(id));
        }
    }
    cJSON_Delete(root);
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& id : ids) if (seen.insert(id).second) out.push_back(id);
    lunar::diagnosticLog("xbox-api", "New cloud product ids count=%zu", out.size());
    return out;
}

std::vector<CloudTitle> XboxApiClient::getHydratedCloudLibrary(
    std::vector<CloudTitle>* recent_out,
    std::vector<CloudTitle>* new_out,
    HttpClient::CancelCallback cancel) {
    last_error_.clear();

    auto v2_titles = getCloudTitles(cancel);
    std::string library_error = last_error_;
    std::vector<CloudTitle> recent_raw;
    bool used_recent_fallback = false;
    if (v2_titles.empty()) {
        recent_raw = getRecentCloudTitles(cancel);
        if (!recent_raw.empty()) {
            lunar::diagnosticLog("xbox-api",
                                 "Full /v2/titles empty/failed; using recent titles as base count=%zu",
                                 recent_raw.size());
            v2_titles = recent_raw;
            used_recent_fallback = true;
            last_error_.clear();
            library_error.clear();
        } else if (!last_error_.empty()) {
            library_error = last_error_;
        }
    } else {
        recent_raw = getRecentCloudTitles(cancel);
    }
    auto new_ids = getNewCloudProductIds(cancel);

    std::map<std::string, CloudTitle> v2_by_product;
    std::vector<std::string> product_ids;
    product_ids.reserve(v2_titles.size());
    for (const auto& title : v2_titles) {
        if (title.product_id.empty()) continue;
        const std::string key = normalizeProductId(title.product_id);
        v2_by_product[key] = title;
        product_ids.push_back(key);
    }

    // Official product expansion is expensive on Switch/emulator networks.
    // Only expand when the account library is still small.
    std::vector<std::string> official;
    if (catalog_base_url_.empty() && !used_recent_fallback && product_ids.size() < 80) {
        official = loadOfficialProductIds();
        if (official.empty()) {
            const std::string titles_url = constants::XSTREAMING_TITLES_JSON_URL;
            auto resp = http_.get(titles_url, {{"Accept", "application/json"}}, cancel);
            if (resp.status_code == 200) {
                cJSON* root = cJSON_Parse(resp.body.c_str());
                if (root) {
                    cJSON* products = cJSON_GetObjectItem(root, "Products");
                    if (products && cJSON_IsArray(products)) {
                        const int count = cJSON_GetArraySize(products);
                        for (int i = 0; i < count; ++i) {
                            cJSON* item = cJSON_GetArrayItem(products, i);
                            if (item && cJSON_IsString(item) && item->valuestring) {
                                official.push_back(normalizeProductId(item->valuestring));
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        std::set<std::string> seen(product_ids.begin(), product_ids.end());
        for (const auto& id : official) {
            if (seen.insert(id).second) product_ids.push_back(id);
        }
    } else if (!used_recent_fallback && product_ids.size() >= 80) {
        lunar::diagnosticLog("xbox-api",
                             "Skipping official titles expansion; account library already large count=%zu",
                             product_ids.size());
    }

    // Prioritize the cards the user sees first. One catalog request can hydrate
    // up to 100 products, so recent, new, and the first library row fit in a
    // bounded 80-title request without hydrating the complete 2k+ catalog.
    constexpr size_t kMaxCatalogHydrate = 80;
    std::vector<std::string> hydrate_ids;
    hydrate_ids.reserve(std::min(product_ids.size(), kMaxCatalogHydrate));
    std::set<std::string> hydrate_seen;
    auto append_hydrate_id = [&](const std::string& id) {
        if (hydrate_ids.size() >= kMaxCatalogHydrate || id.empty()) return;
        const std::string normalized = normalizeProductId(id);
        if (hydrate_seen.insert(normalized).second) {
            hydrate_ids.push_back(normalized);
        }
    };
    for (const auto& title : recent_raw) append_hydrate_id(title.product_id);
    for (const auto& id : new_ids) append_hydrate_id(id);
    for (const auto& id : product_ids) append_hydrate_id(id);
    if (product_ids.size() > hydrate_ids.size()) {
        lunar::diagnosticLog("xbox-api",
                             "Prioritized catalog hydration count=%zu total=%zu recent=%zu new=%zu",
                             hydrate_ids.size(), product_ids.size(),
                             recent_raw.size(), new_ids.size());
    }

    std::vector<CloudTitle> hydrated =
        hydrateCatalogProducts(hydrate_ids, v2_by_product, cancel);

    std::map<std::string, CloudTitle> hydrated_by_product;
    std::map<std::string, CloudTitle> hydrated_by_title;
    for (const auto& title : hydrated) {
        if (!title.product_id.empty()) {
            hydrated_by_product[normalizeProductId(title.product_id)] = title;
        }
        if (!title.title_id.empty()) {
            hydrated_by_title[title.title_id] = title;
        }
    }

    std::vector<CloudTitle> library;
    library.reserve(std::max(v2_titles.size(), hydrated.size()));
    if (!v2_titles.empty()) {
        for (auto title : v2_titles) {
            if (!title.product_id.empty()) {
                auto it = hydrated_by_product.find(normalizeProductId(title.product_id));
                if (it != hydrated_by_product.end()) {
                    auto merged = it->second;
                    if (merged.title_id.empty()) merged.title_id = title.title_id;
                    if (merged.name.empty()) merged.name = title.name;
                    title = std::move(merged);
                }
            } else if (!title.title_id.empty()) {
                auto it = hydrated_by_title.find(title.title_id);
                if (it != hydrated_by_title.end()) {
                    auto merged = it->second;
                    if (merged.title_id.empty()) merged.title_id = title.title_id;
                    if (merged.name.empty()) merged.name = title.name;
                    title = std::move(merged);
                }
            }
            if (title.name.empty()) {
                title.name = !title.title_id.empty() ? title.title_id : title.product_id;
            }
            if (!title.title_id.empty() || !title.product_id.empty()) {
                library.push_back(std::move(title));
            }
        }
    } else {
        library = hydrated;
    }

    if (library.empty() && !v2_titles.empty()) {
        library = v2_titles;
        last_error_.clear();
        lunar::diagnosticLog("xbox-api",
                             "Catalog hydration empty; falling back to raw titles count=%zu recent_fallback=%s",
                             library.size(), used_recent_fallback ? "yes" : "no");
    }

    std::sort(library.begin(), library.end(),
              [](const CloudTitle& a, const CloudTitle& b) { return a.name < b.name; });
    lunar::diagnosticLog("xbox-api",
                         "Library ready count=%zu hydrated=%zu recent_fallback=%s",
                         library.size(), hydrated.size(),
                         used_recent_fallback ? "yes" : "no");

    if (library.empty()) {
        last_error_ = library_error.empty()
            ? "No xCloud library titles found. Full library download may have timed out; try again."
            : library_error;
        return library;
    }

    if (recent_out) {
        recent_out->clear();
        std::map<std::string, CloudTitle> by_product;
        std::map<std::string, CloudTitle> by_title;
        for (const auto& title : library) {
            if (!title.product_id.empty()) by_product[normalizeProductId(title.product_id)] = title;
            if (!title.title_id.empty()) by_title[title.title_id] = title;
        }
        for (const auto& recent : recent_raw) {
            CloudTitle resolved;
            if (!recent.product_id.empty()) {
                auto it = by_product.find(normalizeProductId(recent.product_id));
                if (it != by_product.end()) resolved = it->second;
            }
            if (resolved.title_id.empty() && !recent.title_id.empty()) {
                auto it = by_title.find(recent.title_id);
                if (it != by_title.end()) resolved = it->second;
            }
            if (resolved.title_id.empty()) resolved = recent;
            resolved.is_recent = true;
            if (!resolved.title_id.empty()) recent_out->push_back(std::move(resolved));
        }
    }

    if (new_out) {
        new_out->clear();
        std::map<std::string, CloudTitle> by_product;
        for (const auto& title : library) {
            if (!title.product_id.empty()) by_product[normalizeProductId(title.product_id)] = title;
        }
        for (const auto& id : new_ids) {
            auto it = by_product.find(normalizeProductId(id));
            if (it != by_product.end()) new_out->push_back(it->second);
        }
    }

    lunar::diagnosticLog("xbox-api", "Hydrated cloud library count=%zu recent=%zu new=%zu",
                         library.size(),
                         recent_out ? recent_out->size() : 0,
                         new_out ? new_out->size() : 0);
    return library;
}


} // namespace lunar::api
