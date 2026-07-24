#pragma once

#include "../common.h"
#include "http_client.h"
#include <string>
#include <vector>
#include <map>

namespace lunar::api {

enum class GssvSessionKind {
    Home,
    Cloud,
};

inline const char* gssvSessionKindPath(GssvSessionKind kind) {
    return kind == GssvSessionKind::Cloud ? "cloud" : "home";
}

struct XboxConsole {
    std::string id;
    std::string name;
    std::string console_type;     // "XboxSeriesS", "XboxOne", etc.
    std::string power_state;      // "ConnectedStandby", "On", etc.
    std::string locale;
};

struct CloudTitle {
    std::string title_id;     // used for /play (titleId or XCloudTitleId)
    std::string product_id;   // catalog / store product id
    std::string name;         // ProductTitle or fallback
    std::string image_url;    // poster/tile URL when available
    std::string publisher;
    bool is_recent = false;
};

struct SessionConfig {
    std::string ip_address;
    uint16_t port = 9002;  // Xbox remote play default
    std::string srtp_key;
    std::string ice_exchange_path;
    int keep_alive_seconds = 300;
};

struct CreateSessionRequest {
    GssvSessionKind kind = GssvSessionKind::Home;
    std::string server_id;   // home
    std::string title_id;    // cloud
    int width = 1280;
    int height = 720;
    std::string os_name;     // empty = derive from height
};

class XboxApiClient {
public:
    XboxApiClient(HttpClient& http, const std::string& web_token,
                  const std::string& user_hash, const std::string& gssv_token);

    // Console discovery
    std::vector<XboxConsole> getConsoles(HttpClient::CancelCallback cancel = {});

    // Cloud title catalog (requires cloud base_url + cloud gsToken)
    std::vector<CloudTitle> getRecentCloudTitles(HttpClient::CancelCallback cancel = {});
    std::vector<CloudTitle> getCloudTitles(HttpClient::CancelCallback cancel = {});
    // XStreaming-aligned library: /v2/titles + official productIds + catalog hydration.
    // recent_out optional: recent titles resolved against the full library.
    std::vector<CloudTitle> getHydratedCloudLibrary(
        std::vector<CloudTitle>* recent_out = nullptr,
        std::vector<CloudTitle>* new_out = nullptr,
        HttpClient::CancelCallback cancel = {});
    // product ids from Game Pass "new" sigls feed
    std::vector<std::string> getNewCloudProductIds(HttpClient::CancelCallback cancel = {});

    // Session management
    std::string createSession(const std::string& server_id, int width = 1280, int height = 720,
                              HttpClient::CancelCallback cancel = {});
    std::string createSession(const CreateSessionRequest& request,
                              HttpClient::CancelCallback cancel = {});
    std::string pollSessionState(const std::string& session_id, HttpClient::CancelCallback cancel = {});
    SessionConfig getSessionConfig(const std::string& session_id, HttpClient::CancelCallback cancel = {});

    // Cloud ReadyToConnect → POST /connect with MSAL access token
    bool sendConnect(const std::string& session_id, const std::string& user_token,
                     HttpClient::CancelCallback cancel = {});

    // SDP exchange
    bool sendSdpOffer(const std::string& session_id, const std::string& sdp,
                      HttpClient::CancelCallback cancel = {});
    std::string getSdpAnswer(const std::string& session_id, HttpClient::CancelCallback cancel = {});

    // ICE exchange
    bool sendIceCandidates(const std::string& session_id, const std::string& ice_json,
                           HttpClient::CancelCallback cancel = {});
    std::string getIceCandidates(const std::string& session_id, HttpClient::CancelCallback cancel = {});

    // Session lifecycle
    bool sendKeepAlive(const std::string& session_id, HttpClient::CancelCallback cancel = {});
    bool deleteSession(const std::string& session_id, HttpClient::CancelCallback cancel = {});
    bool cleanupActiveSessions(GssvSessionKind kind,
                               HttpClient::CancelCallback cancel = {});
    void setBaseUrl(const std::string& base_url);
    // When set, catalog.gamepass.com / titles.json calls are rewritten to this base
    // (used by local mock_xbox server). Empty means real Microsoft hosts.
    void setCatalogBaseUrl(const std::string& base_url) { catalog_base_url_ = base_url; }
    void setSessionKind(GssvSessionKind kind) { session_kind_ = kind; }
    GssvSessionKind getSessionKind() const { return session_kind_; }
    std::string getSessionId() const { return session_id_; }
    std::string getLastError() const { return last_error_; }

    // Update tokens after refresh
    void updateTokens(const std::string& web_token, const std::string& gssv_token) {
        web_token_ = web_token;
        gssv_token_ = gssv_token;
    }

private:
    std::string sessionBasePath() const;
    std::vector<CloudTitle> parseCloudTitleList(const std::string& body);
    std::vector<std::string> loadOfficialProductIds() const;
    std::vector<CloudTitle> hydrateCatalogProducts(
        const std::vector<std::string>& product_ids,
        const std::map<std::string, CloudTitle>& v2_by_product,
        HttpClient::CancelCallback cancel);
    static std::string normalizeProductId(std::string id);

    HttpClient& http_;
    std::string web_token_;
    std::string user_hash_;
    std::string gssv_token_;
    std::string base_url_;
    std::string catalog_base_url_;
    GssvSessionKind session_kind_ = GssvSessionKind::Home;
    std::string session_id_;
    std::string last_error_;
};

} // namespace lunar::api
