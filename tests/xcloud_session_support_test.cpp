#include "../src/app/stream_profile.h"
#include "../src/api/xbox_api_client.h"
#include "../src/auth/auth_manager.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

} // namespace

int main() {
    auto home = lunar::app::makeHomeStreamProfile("console-1", 1280, 720);
    require(home.type == lunar::app::SessionType::Home, "home type");
    require(home.server_id == "console-1", "home server id");
    require(home.title_id.empty(), "home title empty");
    require(!home.prefer_ipv6, "home prefer lan");

    auto cloud = lunar::app::makeCloudStreamProfile("TITLE123", 1920, 1080);
    require(cloud.type == lunar::app::SessionType::Cloud, "cloud type");
    require(cloud.title_id == "TITLE123", "cloud title id");
    require(cloud.server_id.empty(), "cloud server empty");
    require(cloud.prefer_ipv6, "cloud prefer public/ipv6");
    require(cloud.os_name == "windows", "cloud 1080 os");

    auto cloud_hq = lunar::app::makeCloudStreamProfile(
        "TITLE123", 1920, 1080, 30000);
    require(cloud_hq.bitrate_kbps == 30000, "cloud 1080 HQ bitrate");
    require(cloud_hq.os_name == "windows", "cloud 1080 HQ stays strict 1080p");

    require(std::string(lunar::api::gssvSessionKindPath(lunar::api::GssvSessionKind::Home)) == "home",
            "home path");
    require(std::string(lunar::api::gssvSessionKindPath(lunar::api::GssvSessionKind::Cloud)) == "cloud",
            "cloud path");

    lunar::auth::StreamingToken token;
    require(!token.valid(), "empty token invalid");
    token.gs_token = "abc";
    require(token.valid(), "token with gsToken valid");

    lunar::api::CloudTitle title;
    title.title_id = "HALOINFINITE";
    title.product_id = "9NP1P1WFS0LB";
    title.name = "Halo Infinite";
    title.is_recent = true;
    require(title.is_recent, "recent flag");
    require(!title.product_id.empty(), "product id");

    std::cout << "xcloud_session_support_test OK\n";
    return 0;
}
