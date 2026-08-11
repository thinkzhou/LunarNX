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
    int fps = 60;
    // Receiver capability advertised after the message-channel handshake.
    // Zero selects the resolution-based default (10 Mbps for 720p, 20 Mbps
    // for 1080p); callers can use 30000 for the 1080p HQ tier.
    int bitrate_kbps = 0;
    std::string os_name = "windows";
    std::string locale = "en-US";
    bool prefer_ipv6 = false;
    std::string base_url;  // Override GSSV API base URL (empty = use default)
    // MSAL access token used for cloud ReadyToConnect → /connect (XStreaming "lpt").
    std::string msal_user_token;
};

inline StreamProfile makeHomeStreamProfile(const std::string& server_id,
                                           int width,
                                           int height,
                                           int bitrate_kbps = 0) {
    StreamProfile profile;
    profile.type = SessionType::Home;
    profile.server_id = server_id;
    profile.width = width;
    profile.height = height;
    profile.bitrate_kbps = bitrate_kbps > 0
        ? bitrate_kbps
        : (height >= 1080 ? 20000 : 10000);
    profile.os_name = height >= 1080 ? "windows" : "android";
    profile.prefer_ipv6 = false;
    return profile;
}

inline StreamProfile makeCloudStreamProfile(const std::string& title_id,
                                            int width,
                                            int height,
                                            int bitrate_kbps = 0) {
    StreamProfile profile;
    profile.type = SessionType::Cloud;
    profile.title_id = title_id;
    profile.width = width;
    profile.height = height;
    profile.bitrate_kbps = bitrate_kbps > 0
        ? bitrate_kbps
        : (height >= 1080 ? 20000 : 10000);
    // xCloud uses the Tizen device fingerprint for its 1080p high-bitrate
    // tier. Ordinary 1080p remains the Windows profile for compatibility.
    profile.os_name = profile.bitrate_kbps >= 30000 && height >= 1080
        ? "tizen"
        : (height >= 1080 ? "windows" : "android");
    // Cloud is WAN-only; prefer public IPv4 / IPv6 over LAN private ranks.
    profile.prefer_ipv6 = true;
    return profile;
}

inline int streamProfileBitrateKbps(const StreamProfile& profile) {
    if (profile.bitrate_kbps > 0) return profile.bitrate_kbps;
    return profile.height >= 1080 ? 20000 : 10000;
}

} // namespace lunar::app
