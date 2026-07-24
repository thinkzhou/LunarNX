#include "../api/api_constants.h"
#include "../api/http_client.h"
#include "../api/xbox_api_client.h"
#include "../app/ice_candidate_processor.h"
#include "../app/stream_profile.h"
#include "../auth/auth_manager.h"
#include "../webrtc/peer_manager.h"

#include <cJSON.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using lunar::api::HttpClient;
using lunar::api::HttpResponse;
using lunar::api::XboxApiClient;

enum class SessionVariant {
    Current,
    LegacyIce,
    GreenlightBrowserNoIce,
};

enum class PayloadVariant {
    Current,
    CurrentWithRequestId,
    XStreaming,
    Greenlight,
};

enum class HeaderVariant {
    Current,
    Greenlight,
};

enum class SdpVariant {
    LibPeer,
    ActPass,
};

struct ProbeCase {
    std::string name;
    SessionVariant session;
    PayloadVariant payload;
    HeaderVariant headers;
    SdpVariant sdp;
};

std::string getEnvOr(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value && value[0] ? value : fallback;
}

std::string defaultTokenPath() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) {
        return "token.json";
    }
    return std::string(home) + "/work/self/ryujinx-data/sdcard/switch/LunarNX/token.json";
}

std::string oneLine(std::string value, size_t max_len = 2048) {
    for (char& c : value) {
        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }
    }
    if (value.size() > max_len) {
        value.resize(max_len);
        value += "...";
    }
    return value;
}

std::string jsonToString(cJSON* root) {
    char* raw = cJSON_PrintUnformatted(root);
    std::string out = raw ? raw : "";
    std::free(raw);
    return out;
}

void writeTextFile(const std::string& path, const std::string& value) {
    std::ofstream out(path, std::ios::binary);
    out << value;
}

std::string replaceAll(std::string value, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string summarizeSdp(const std::string& sdp) {
    auto has = [&sdp](const char* needle) {
        return sdp.find(needle) != std::string::npos ? "yes" : "no";
    };

    std::ostringstream out;
    out << "len=" << sdp.size()
        << " video=" << has("m=video")
        << " audio=" << has("m=audio")
        << " data=" << has("m=application")
        << " sctp=" << has("a=sctp-port")
        << " passive=" << has("a=setup:passive")
        << " actpass=" << has("a=setup:actpass")
        << " recvonly=" << has("a=recvonly")
        << " sendrecv=" << has("a=sendrecv");
    return out.str();
}

void addVersion(cJSON* parent, const char* key, int min_version, int max_version) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "minVersion", min_version);
    cJSON_AddNumberToObject(obj, "maxVersion", max_version);
    cJSON_AddItemToObject(parent, key, obj);
}

std::string makeDeviceInfo(SessionVariant variant, int width, int height) {
    cJSON* root = cJSON_CreateObject();
    cJSON* app_info = cJSON_CreateObject();
    cJSON* env = cJSON_CreateObject();

    const bool current = variant == SessionVariant::Current;
    const bool browser = current || variant == SessionVariant::GreenlightBrowserNoIce;
    cJSON_AddStringToObject(env, "clientAppId", browser ? "www.xbox.com" : "Microsoft.GamingApp");
    cJSON_AddStringToObject(env, "clientAppType", browser ? "browser" : "native");
    cJSON_AddStringToObject(env,
                            "clientAppVersion",
                            current ? "29.9.35" : (browser ? "21.1.98" : "2203.1001.4.0"));
    cJSON_AddStringToObject(env,
                            "clientSdkVersion",
                            current ? "10.6.8" : (browser ? "8.5.3" : "8.5.2"));
    cJSON_AddStringToObject(env, "httpEnvironment", "prod");
    cJSON_AddStringToObject(env, "sdkInstallId", "");
    cJSON_AddItemToObject(app_info, "env", env);
    cJSON_AddItemToObject(root, "appInfo", app_info);

    cJSON* dev = cJSON_CreateObject();
    cJSON* hw = cJSON_CreateObject();
    cJSON_AddStringToObject(hw, "make", "Microsoft");
    cJSON_AddStringToObject(hw, "model", browser ? "unknown" : "Surface Pro");
    if (browser) {
        cJSON_AddStringToObject(hw, "platformType", "desktop");
    }
    cJSON_AddStringToObject(hw, "sdktype", browser ? "web" : "native");
    cJSON_AddItemToObject(dev, "hw", hw);

    cJSON* os = cJSON_CreateObject();
    cJSON_AddStringToObject(os,
                            "name",
                            browser ? (height >= 1080 ? "windows" : "android")
                                    : "Windows 11");
    cJSON_AddStringToObject(os, "ver", "22631.2715");
    cJSON_AddStringToObject(os, "platform", "desktop");
    cJSON_AddItemToObject(dev, "os", os);

    cJSON* display = cJSON_CreateObject();
    cJSON* dims = cJSON_CreateObject();
    cJSON_AddNumberToObject(dims, "widthInPixels", width);
    cJSON_AddNumberToObject(dims, "heightInPixels", height);
    cJSON_AddItemToObject(display, "dimensions", dims);

    cJSON* pixel = cJSON_CreateObject();
    cJSON_AddNumberToObject(pixel, "dpiX", browser ? 2 : 1);
    cJSON_AddNumberToObject(pixel, "dpiY", browser ? 2 : 1);
    cJSON_AddItemToObject(display, "pixelDensity", pixel);
    cJSON_AddItemToObject(dev, "displayInfo", display);

    if (browser) {
        cJSON* browser_info = cJSON_CreateObject();
        cJSON_AddStringToObject(browser_info, "browserName", current ? "edge" : "chrome");
        cJSON_AddStringToObject(browser_info,
                                "browserVersion",
                                current ? "140.0.3485.66" : "119.0");
        cJSON_AddItemToObject(dev, "browser", browser_info);
    }

    cJSON_AddItemToObject(root, "dev", dev);
    std::string out = jsonToString(root);
    cJSON_Delete(root);
    return out;
}

std::string makeSessionBody(const std::string& server_id,
                            SessionVariant variant,
                            int width,
                            int height) {
    cJSON* settings = cJSON_CreateObject();
    cJSON_AddStringToObject(settings, "nanoVersion", "V3;WebrtcTransport.dll");
    if (variant == SessionVariant::GreenlightBrowserNoIce) {
        cJSON_AddBoolToObject(settings, "enableOptionalDataCollection", false);
    }
    cJSON_AddBoolToObject(settings, "enableTextToSpeech", false);
    cJSON_AddNumberToObject(settings, "highContrast", 0);
    cJSON_AddStringToObject(settings, "locale", "en-US");
    cJSON_AddBoolToObject(settings, "useIceConnection", variant == SessionVariant::LegacyIce);
    cJSON_AddNumberToObject(settings,
                            "timezoneOffsetMinutes",
                            variant == SessionVariant::LegacyIce ? 480 : 120);
    cJSON_AddStringToObject(settings, "sdkType", "web");
    cJSON_AddStringToObject(settings, "osName", height >= 1080 ? "windows" : "android");

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "clientSessionId", "");
    cJSON_AddStringToObject(body, "titleId", "");
    cJSON_AddStringToObject(body, "systemUpdateGroup", "");
    cJSON_AddItemToObject(body, "settings", settings);
    cJSON_AddStringToObject(body, "serverId", server_id.c_str());
    cJSON_AddItemToObject(body, "fallbackRegionNames", cJSON_CreateArray());

    std::string out = jsonToString(body);
    cJSON_Delete(body);
    return out;
}

std::map<std::string, std::string> makeSessionHeaders(const std::string& token,
                                                      const std::string& device_info,
                                                      HeaderVariant header_variant) {
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["X-MS-Device-Info"] = device_info;
    headers["Authorization"] = "Bearer " + token;
    if (header_variant == HeaderVariant::Greenlight) {
        headers["Accept"] = "application/json";
        headers["X-Gssv-Client"] = "XboxComBrowser";
    }
    return headers;
}

std::string makeSdpBody(const std::string& sdp, PayloadVariant variant) {
    cJSON* chat_config = cJSON_CreateObject();
    cJSON_AddNumberToObject(chat_config, "bytesPerSample", 2);
    cJSON_AddNumberToObject(chat_config, "expectedClipDurationMs", 20);
    cJSON* format = cJSON_CreateObject();
    cJSON_AddStringToObject(format, "codec", "opus");
    cJSON_AddStringToObject(format, "container", "webm");
    cJSON_AddItemToObject(chat_config, "format", format);
    cJSON_AddNumberToObject(chat_config, "numChannels", 1);
    cJSON_AddNumberToObject(chat_config, "sampleFrequencyHz", 24000);

    cJSON* config = cJSON_CreateObject();
    cJSON_AddItemToObject(config, "chatConfiguration", chat_config);
    addVersion(config, "chat", 1, 1);
    addVersion(config, "control", 1, 3);
    addVersion(config,
               "input",
               1,
               variant == PayloadVariant::XStreaming ? 8 : 9);
    addVersion(config, "message", 1, 1);
    if (variant == PayloadVariant::Current ||
        variant == PayloadVariant::CurrentWithRequestId ||
        variant == PayloadVariant::Greenlight) {
        addVersion(config, "reliableinput", 9, 9);
        addVersion(config, "unreliableinput", 9, 9);
    }

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "messageType", "offer");
    cJSON_AddStringToObject(body, "sdp", sdp.c_str());
    if (variant == PayloadVariant::CurrentWithRequestId ||
        variant == PayloadVariant::Greenlight) {
        cJSON_AddStringToObject(body, "requestId", "1");
    }
    cJSON_AddItemToObject(body, "configuration", config);

    std::string out = jsonToString(body);
    cJSON_Delete(body);
    return out;
}

std::map<std::string, std::string> makeSdpHeaders(const std::string& token,
                                                  const std::string& device_info,
                                                  HeaderVariant header_variant) {
    std::map<std::string, std::string> headers;
    headers["Accept"] = "application/json";
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer " + token;
    if (header_variant == HeaderVariant::Greenlight) {
        headers["X-Gssv-Client"] = "XboxComBrowser";
        headers["X-MS-Device-Info"] = device_info;
    }
    return headers;
}

std::string parseStringField(const std::string& json, const char* field) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        return "";
    }
    std::string out;
    cJSON* item = cJSON_GetObjectItem(root, field);
    if (item && cJSON_IsString(item) && item->valuestring) {
        out = item->valuestring;
    }
    cJSON_Delete(root);
    return out;
}

std::string createSession(HttpClient& http,
                          const std::string& token,
                          const std::string& server_id,
                          const ProbeCase& probe,
                          int width,
                          int height) {
    const std::string device_info = makeDeviceInfo(probe.session, width, height);
    const std::string body = makeSessionBody(server_id, probe.session, width, height);
    const std::string url =
        std::string(lunar::api::constants::GSSV_HOME_BASE) + "/v5/sessions/home/play";
    auto resp = http.post(url,
                          body,
                          makeSessionHeaders(token, device_info, probe.headers));

    std::cout << "[" << probe.name << "] create session status=" << resp.status_code
              << " network=" << (resp.network_error ? "true" : "false")
              << " body=" << oneLine(resp.body, 512) << "\n";

    if (resp.status_code < 200 || resp.status_code >= 300) {
        return "";
    }
    return parseStringField(resp.body, "sessionId");
}

std::string getSessionState(HttpClient& http,
                            const std::string& token,
                            const std::string& session_id,
                            const std::string& probe_name) {
    const std::string url = std::string(lunar::api::constants::GSSV_HOME_BASE) +
        "/v5/sessions/home/" + session_id + "/state";
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["Authorization"] = "Bearer " + token;
    auto resp = http.get(url, headers);
    std::cout << "[" << probe_name << "] state status=" << resp.status_code
              << " body=" << oneLine(resp.body, 768) << "\n";
    if (resp.status_code != 200) {
        return "Error";
    }
    return parseStringField(resp.body, "state");
}

bool waitProvisioned(HttpClient& http,
                     const std::string& token,
                     const std::string& session_id,
                     const std::string& probe_name) {
    for (int i = 0; i < 30; ++i) {
        std::string state = getSessionState(http, token, session_id, probe_name);
        std::cout << "  state[" << i << "]=" << state << "\n";
        if (state == "Provisioned") {
            return true;
        }
        if (state == "Error" || state == "Failed") {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

std::string makeOffer(SdpVariant variant, const std::string& sdp_file) {
    std::string offer;
    if (!sdp_file.empty()) {
        offer = readFile(sdp_file);
    }

    if (variant == SdpVariant::ActPass) {
        offer = replaceAll(offer, "a=setup:passive", "a=setup:actpass");
    }
    return offer;
}

std::string extractAnswerSdp(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return "";
    }

    std::string exchange;
    cJSON* exchange_response = cJSON_GetObjectItem(root, "exchangeResponse");
    if (exchange_response && cJSON_IsString(exchange_response) &&
        exchange_response->valuestring) {
        exchange = exchange_response->valuestring;
    }
    cJSON_Delete(root);

    if (exchange.empty()) {
        return "";
    }

    cJSON* inner = cJSON_Parse(exchange.c_str());
    if (!inner) {
        return exchange;
    }

    std::string sdp;
    cJSON* sdp_item = cJSON_GetObjectItem(inner, "sdp");
    if (sdp_item && cJSON_IsString(sdp_item) && sdp_item->valuestring) {
        sdp = sdp_item->valuestring;
    }
    cJSON_Delete(inner);
    return sdp;
}

bool runProbeCase(HttpClient& http,
                  XboxApiClient& api,
                  const std::string& token,
                  const std::string& server_id,
                  const ProbeCase& probe,
                  const std::string& sdp_file,
                  int width,
                  int height) {
    const std::string session_id = createSession(http, token, server_id, probe, width, height);
    if (session_id.empty()) {
        std::cerr << "[" << probe.name << "] no session id\n";
        return false;
    }

    bool ok = false;
    auto cleanup = [&]() {
        const bool deleted = api.deleteSession(session_id);
        std::cout << "[" << probe.name << "] delete session " << (deleted ? "ok" : "failed")
                  << "\n";
    };

    if (!waitProvisioned(http, token, session_id, probe.name)) {
        std::cerr << "[" << probe.name << "] session did not provision\n";
        cleanup();
        return false;
    }

    auto config = api.getSessionConfig(session_id);
    std::cout << "[" << probe.name << "] config ip=" << config.ip_address
              << " port=" << config.port
              << " keep_alive=" << config.keep_alive_seconds << "\n";

    lunar::webrtc::PeerManager peer;
    std::string offer;
    if (sdp_file.empty()) {
        if (!peer.initialize()) {
            std::cerr << "[" << probe.name << "] peer initialize failed\n";
            cleanup();
            return false;
        }
        offer = peer.createOffer();
    } else {
        offer = makeOffer(probe.sdp, sdp_file);
    }
    if (probe.sdp == SdpVariant::ActPass) {
        offer = replaceAll(offer, "a=setup:passive", "a=setup:actpass");
    }
    if (offer.empty()) {
        cleanup();
        return false;
    }
    std::cout << "[" << probe.name << "] offer " << summarizeSdp(offer) << "\n";

    const std::string device_info = makeDeviceInfo(probe.session, width, height);
    const std::string body = makeSdpBody(offer, probe.payload);
    const std::string base = "/tmp/lunarnx_sdp_probe_" + probe.name;
    writeTextFile(base + "_offer.sdp", offer);
    writeTextFile(base + "_request.json", body);

    const std::string url = std::string(lunar::api::constants::GSSV_HOME_BASE) +
        "/v5/sessions/home/" + session_id + "/sdp";
    auto resp = http.post(url,
                          body,
                          makeSdpHeaders(token, device_info, probe.headers));
    writeTextFile(base + "_response.txt", resp.body);
    std::cout << "[" << probe.name << "] sdp POST status=" << resp.status_code
              << " network=" << (resp.network_error ? "true" : "false")
              << " body_len=" << resp.body.size()
              << " body=" << oneLine(resp.body) << "\n";

    std::string answer_sdp;
    if (resp.status_code >= 200 && resp.status_code < 300) {
        for (int i = 0; i < 20; ++i) {
            auto answer = http.get(url, makeSdpHeaders(token, device_info, probe.headers));
            writeTextFile(base + "_answer.json", answer.body);
            std::cout << "[" << probe.name << "] sdp GET[" << i << "] status="
                      << answer.status_code
                      << " body_len=" << answer.body.size()
                      << " body=" << oneLine(answer.body, 1024) << "\n";
            if (answer.status_code >= 200 && answer.status_code < 300 &&
                answer.status_code != 204 && !answer.body.empty()) {
                answer_sdp = extractAnswerSdp(answer.body);
                ok = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    if (ok) {
        if (!answer_sdp.empty() && sdp_file.empty()) {
            peer.setRemoteAnswer(answer_sdp);
        }

        lunar::app::IceCandidateProcessor ice_processor;
        const auto local_candidates = sdp_file.empty()
            ? peer.getLocalCandidates()
            : std::vector<lunar::webrtc::IceCandidate>{};
        const auto ice_payloads = ice_processor.fromLocal(
            local_candidates,
            lunar::app::IceCandidateProcessor::usernameFragmentFromSdp(offer));
        const std::string ice_body = ice_processor.toApiJson(ice_payloads);
        writeTextFile(base + "_ice_request.json", ice_body);
        std::cout << "[" << probe.name << "] local ICE count=" << local_candidates.size()
                  << " api_count=" << ice_payloads.size()
                  << " body=" << oneLine(ice_body, 1024) << "\n";

        const std::string ice_url = std::string(lunar::api::constants::GSSV_HOME_BASE) +
            "/v5/sessions/home/" + session_id + "/ice";
        auto send_ice = http.post(ice_url,
                                  ice_body,
                                  makeSdpHeaders(token, device_info, probe.headers));
        writeTextFile(base + "_ice_response.txt", send_ice.body);
        std::cout << "[" << probe.name << "] ice POST status=" << send_ice.status_code
                  << " network=" << (send_ice.network_error ? "true" : "false")
                  << " body_len=" << send_ice.body.size()
                  << " body=" << oneLine(send_ice.body, 1024) << "\n";

        bool ice_ok = send_ice.status_code >= 200 && send_ice.status_code < 300;
        for (int i = 0; i < 20 && ice_ok; ++i) {
            auto remote_ice = http.get(ice_url, makeSdpHeaders(token, device_info, probe.headers));
            writeTextFile(base + "_remote_ice.json", remote_ice.body);
            std::cout << "[" << probe.name << "] ice GET[" << i << "] status="
                      << remote_ice.status_code
                      << " body_len=" << remote_ice.body.size()
                      << " body=" << oneLine(remote_ice.body, 1024) << "\n";
            if (remote_ice.status_code >= 200 && remote_ice.status_code < 300 &&
                remote_ice.status_code != 204 && !remote_ice.body.empty()) {
                auto parsed = ice_processor.parseRemotePayload(
                    remote_ice.body,
                    lunar::app::makeHomeStreamProfile(server_id, width, height));
                std::cout << "[" << probe.name << "] remote ICE parsed count="
                          << parsed.size() << "\n";
                std::ostringstream rewritten_dump;
                for (size_t idx = 0; idx < parsed.size(); ++idx) {
                    const auto& candidate = parsed[idx];
                    std::cout << "[" << probe.name << "] remote rewritten["
                              << idx << "] " << candidate.candidate << "\n";
                    rewritten_dump << candidate.candidate << "\n";
                }
                writeTextFile(base + "_remote_ice_rewritten.txt",
                              rewritten_dump.str());
                ice_ok = !parsed.empty();
                if (ice_ok && sdp_file.empty()) {
                    for (const auto& candidate : parsed) {
                        if (candidate.candidate != "a=end-of-candidates") {
                            peer.addIceCandidate(candidate.candidate);
                        }
                    }

                    const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(20);
                    auto last_status = std::chrono::steady_clock::now();
                    while (std::chrono::steady_clock::now() < deadline &&
                           !peer.isDataChannelReady()) {
                        peer.processEvents();
                        const auto now = std::chrono::steady_clock::now();
                        if (now - last_status >= std::chrono::seconds(2)) {
                            std::cout << "[" << probe.name
                                      << "] ICE wait connected="
                                      << (peer.isConnected() ? "true" : "false")
                                      << " datachannel="
                                      << (peer.isDataChannelReady() ? "true"
                                                                    : "false")
                                      << "\n";
                            last_status = now;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    std::cout << "[" << probe.name << "] peer connected="
                              << (peer.isConnected() ? "true" : "false")
                              << " datachannel="
                              << (peer.isDataChannelReady() ? "true" : "false")
                              << "\n";
                    ice_ok = peer.isConnected();
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ok = ok && ice_ok;
    }

    peer.disconnect();
    cleanup();
    return ok;
}

std::vector<ProbeCase> allCases() {
    return {
        {"current",
         SessionVariant::Current,
         PayloadVariant::Current,
         HeaderVariant::Current,
         SdpVariant::LibPeer},
        {"legacy-ice",
         SessionVariant::LegacyIce,
         PayloadVariant::Current,
         HeaderVariant::Current,
         SdpVariant::LibPeer},
        {"requestid",
         SessionVariant::Current,
         PayloadVariant::CurrentWithRequestId,
         HeaderVariant::Current,
         SdpVariant::LibPeer},
        {"xstreaming",
         SessionVariant::Current,
         PayloadVariant::XStreaming,
         HeaderVariant::Current,
         SdpVariant::LibPeer},
        {"noice-current",
         SessionVariant::Current,
         PayloadVariant::Current,
         HeaderVariant::Current,
         SdpVariant::LibPeer},
        {"greenlight",
         SessionVariant::GreenlightBrowserNoIce,
         PayloadVariant::Greenlight,
         HeaderVariant::Greenlight,
         SdpVariant::LibPeer},
        {"actpass",
         SessionVariant::GreenlightBrowserNoIce,
         PayloadVariant::Greenlight,
         HeaderVariant::Greenlight,
         SdpVariant::ActPass},
    };
}

void printUsage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [--all] [--case name] [--sdp-file path]\n"
        << "  env LUNARNX_TOKEN_PATH defaults to " << defaultTokenPath() << "\n"
        << "  env LUNARNX_SERVER_ID can skip console discovery\n"
        << "  cases: current, legacy-ice, requestid, xstreaming, noice-current, greenlight, actpass\n";
}

} // namespace

int main(int argc, char** argv) {
    bool run_all = false;
    std::string requested_case = "current";
    std::string sdp_file;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--all") {
            run_all = true;
        } else if (arg == "--case" && i + 1 < argc) {
            requested_case = argv[++i];
        } else if (arg == "--sdp-file" && i + 1 < argc) {
            sdp_file = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }

    const std::string token_path = getEnvOr("LUNARNX_TOKEN_PATH", defaultTokenPath());
    const int width = std::atoi(getEnvOr("LUNARNX_WIDTH", "1280").c_str());
    const int height = std::atoi(getEnvOr("LUNARNX_HEIGHT", "720").c_str());

    HttpClient http;
    lunar::auth::AuthManager auth(http);
    if (!auth.loadTokens(token_path)) {
        std::cerr << "Failed to load token file: " << token_path << "\n";
        return 1;
    }
    if (!auth.refreshTokensIfNeeded() && !auth.isAuthenticated()) {
        std::cerr << "Token refresh/auth failed: " << auth.getLastError() << "\n";
        return 1;
    }
    auth.saveTokens(token_path);

    XboxApiClient api(http, auth.getWebToken(), auth.getUserHash(), auth.getGssvToken());

    std::string server_id = getEnvOr("LUNARNX_SERVER_ID", "");
    if (server_id.empty()) {
        auto consoles = api.getConsoles();
        if (consoles.empty()) {
            std::cerr << "No consoles found: " << api.getLastError() << "\n";
            return 1;
        }
        server_id = consoles.front().id;
        std::cout << "Using console name=" << consoles.front().name
                  << " type=" << consoles.front().console_type
                  << " state=" << consoles.front().power_state << "\n";
    } else {
        std::cout << "Using console from LUNARNX_SERVER_ID\n";
    }

    bool any_ok = false;
    bool matched = false;
    for (const auto& probe : allCases()) {
        if (!run_all && probe.name != requested_case) {
            continue;
        }
        matched = true;
        std::cout << "=== case " << probe.name << " ===\n";
        any_ok = runProbeCase(http,
                              api,
                              auth.getGssvToken(),
                              server_id,
                              probe,
                              sdp_file,
                              width > 0 ? width : 1280,
                              height > 0 ? height : 720) || any_ok;
    }

    if (!matched) {
        std::cerr << "Unknown case: " << requested_case << "\n";
        printUsage(argv[0]);
        return 2;
    }
    return any_ok ? 0 : 3;
}
