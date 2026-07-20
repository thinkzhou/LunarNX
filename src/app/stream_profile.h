#pragma once

#include <string>

namespace lunar::app {

enum class SessionType {
    Home,
    Cloud,
};

struct StreamProfile {
    SessionType type = SessionType::Home;
    std::string server_id;
    std::string title_id;
    int width = 1280;
    int height = 720;
    std::string os_name = "windows";
    bool prefer_ipv6 = false;
    std::string base_url;  // Override GSSV API base URL (empty = use default)
    // MSAL access token used for cloud ReadyToConnect → /connect (XStreaming "lpt").
    std::string msal_user_token;
};

inline StreamProfile makeHomeStreamProfile(const std::string& server_id,
                                           int width,
                                           int height) {
    StreamProfile profile;
    profile.type = SessionType::Home;
    profile.server_id = server_id;
    profile.width = width;
    profile.height = height;
    profile.os_name = height >= 1080 ? "windows" : "android";
    profile.prefer_ipv6 = false;
    return profile;
}

inline StreamProfile makeCloudStreamProfile(const std::string& title_id,
                                            int width,
                                            int height) {
    StreamProfile profile;
    profile.type = SessionType::Cloud;
    profile.title_id = title_id;
    profile.width = width;
    profile.height = height;
    profile.os_name = height >= 1080 ? "windows" : "android";
    // Cloud is WAN-only; prefer public IPv4 / IPv6 over LAN private ranks.
    profile.prefer_ipv6 = true;
    return profile;
}

} // namespace lunar::app
