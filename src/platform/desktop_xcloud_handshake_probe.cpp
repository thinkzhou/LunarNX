// Desktop xCloud protocol handshake probe (no full media loop required).
// Walks: token refresh -> recent titles -> play (skip NoEntitlement)
//        -> ReadyToConnect/connect -> Provisioned/configuration
//        -> SDP offer/answer -> ICE post/get -> delete session
//
// Usage:
//   make -f Makefile.desktop xcloud_handshake_probe
//   LUNARNX_TOKEN_PATH=./token.json \
//   LUNARNX_FORCE_REGION_IP=4.2.2.2 \
//   ./build/pc/xcloud_handshake_probe
//
// Optional:
//   LUNARNX_TITLE_ID=CYBERPUNK2077
//   LUNARNX_WIDTH=1280 LUNARNX_HEIGHT=720

#include "../api/http_client.h"
#include "../api/xbox_api_client.h"
#include "../app/stream_profile.h"
#include "../app/xbox_session_client.h"
#include "../auth/auth_manager.h"
#include "../common.h"
#include "../webrtc/peer_manager.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using clock = std::chrono::steady_clock;

double ms_since(clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

const char* envOr(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return (v && v[0]) ? v : fallback;
}

bool sleepFor(std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
    return true;
}

} // namespace

int main() {
    const char* token_path = envOr("LUNARNX_TOKEN_PATH", lunar::get_token_path());
    const char* region = envOr("LUNARNX_FORCE_REGION_IP", "4.2.2.2");
    const char* title_override = std::getenv("LUNARNX_TITLE_ID");
    const int width = std::atoi(envOr("LUNARNX_WIDTH", "1280"));
    const int height = std::atoi(envOr("LUNARNX_HEIGHT", "720"));

    printf("=== LunarNX xCloud handshake probe ===\n");
    printf("token_path=%s\n", token_path);
    printf("force_region_ip=%s\n", region[0] ? region : "(empty)");
    printf("resolution=%dx%d\n", width, height);

    lunar::api::HttpClient http;
    lunar::auth::AuthManager auth(http);
    auth.setForceRegionIp(region);

    if (!auth.loadTokens(token_path)) {
        fprintf(stderr, "FAIL: loadTokens(%s)\n", token_path);
        return 1;
    }
    printf("loaded gamertag=%s cloud_before=%s\n",
           auth.getGamertag().c_str(),
           auth.hasCloudAccess() ? "yes" : "no");

    auto t0 = clock::now();
    if (!auth.refreshStreamingTokens(true) && !auth.hasCloudAccess()) {
        fprintf(stderr, "FAIL: no cloud token err=%s\n", auth.getLastError().c_str());
        return 2;
    }
    auth.saveTokens(token_path);
    printf("token refresh elapsed_ms=%.0f cloud=%s home=%s\n",
           ms_since(t0),
           auth.hasCloudAccess() ? "yes" : "no",
           auth.getHomeStreamingToken().valid() ? "yes" : "no");
    if (!auth.hasCloudAccess()) {
        fprintf(stderr, "FAIL: cloud token unavailable: %s\n", auth.getLastError().c_str());
        return 2;
    }

    const auto cloud = auth.getCloudStreamingToken();
    printf("cloud_base=%s token_len=%zu\n", cloud.base_uri.c_str(), cloud.gs_token.size());
    const std::string lpt = auth.getXcloudTransferToken(true);
    if (lpt.empty()) {
        fprintf(stderr, "FAIL: xCloud connect token (lpt) empty: %s\n",
                auth.getLastError().c_str());
        return 4;
    }
    printf("xcloud transfer token (lpt) len=%zu\n", lpt.size());

    auto api = std::make_shared<lunar::api::XboxApiClient>(
        http, auth.getWebToken(), auth.getUserHash(), cloud.gs_token);
    api->setSessionKind(lunar::api::GssvSessionKind::Cloud);
    if (!cloud.base_uri.empty()) {
        api->setBaseUrl(cloud.base_uri);
    }

    std::vector<std::pair<std::string, std::string>> candidates;
    if (title_override && title_override[0]) {
        candidates.emplace_back(title_override, title_override);
        printf("using title override: %s\n", title_override);
    } else {
        t0 = clock::now();
        auto recent = api->getRecentCloudTitles();
        printf("recent titles count=%zu elapsed_ms=%.0f err=%s\n",
               recent.size(), ms_since(t0), api->getLastError().c_str());
        if (recent.empty()) {
            fprintf(stderr, "FAIL: no recent titles. Set LUNARNX_TITLE_ID.\n");
            return 3;
        }
        for (const auto& r : recent) {
            candidates.emplace_back(r.title_id, r.name.empty() ? r.title_id : r.name);
        }
    }

    lunar::app::XboxSessionClient session(api);
    lunar::app::ProvisionedSession provisioned;
    std::string chosen_title_id;
    std::string chosen_title_name;

    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& title_id = candidates[i].first;
        const auto& title_name = candidates[i].second;
        printf("try title[%zu]=%s (%s)\n", i, title_name.c_str(), title_id.c_str());

        auto profile = lunar::app::makeCloudStreamProfile(title_id, width, height);
        profile.base_url = cloud.base_uri;
        profile.msal_user_token = lpt;

        t0 = clock::now();
        auto attempt = session.createAndWait(
            profile,
            {},
            [](const std::string& s) { printf("  status: %s\n", s.c_str()); },
            [](std::chrono::milliseconds d) { return sleepFor(d); });

        printf("createAndWait elapsed_ms=%.0f status=%d session_id=%s err=%s\n",
               ms_since(t0),
               static_cast<int>(attempt.status),
               attempt.session_id.c_str(),
               attempt.error.c_str());

        if (!attempt.session_id.empty() &&
            attempt.status == lunar::app::SessionStartStatus::Ok) {
            provisioned = std::move(attempt);
            chosen_title_id = title_id;
            chosen_title_name = title_name;
            break;
        }
        if (!attempt.session_id.empty()) {
            api->deleteSession(attempt.session_id);
        }

        const std::string& err = attempt.error;
        printf("skip title due to session error: %s\n", err.c_str());
        continue;
    }

    if (provisioned.session_id.empty() ||
        provisioned.status != lunar::app::SessionStartStatus::Ok) {
        fprintf(stderr, "FAIL: no playable title among candidates\n");
        return 6;
    }
    printf("chosen title=%s (%s)\n", chosen_title_name.c_str(), chosen_title_id.c_str());
    printf("session config ip=%s port=%u ice_path=%s keep_alive=%d\n",
           provisioned.config.ip_address.c_str(),
           provisioned.config.port,
           provisioned.config.ice_exchange_path.c_str(),
           provisioned.config.keep_alive_seconds);

    lunar::webrtc::PeerManager peer;
    if (!peer.initialize()) {
        fprintf(stderr, "FAIL: PeerManager initialize\n");
        api->deleteSession(provisioned.session_id);
        return 7;
    }
    if (!peer.createDataChannels()) {
        fprintf(stderr, "WARN: createDataChannels failed (continuing SDP probe)\n");
    }

    t0 = clock::now();
    std::string offer = peer.createOffer();
    printf("createOffer elapsed_ms=%.0f offer_len=%zu\n", ms_since(t0), offer.size());
    if (offer.empty()) {
        fprintf(stderr, "FAIL: empty SDP offer\n");
        api->deleteSession(provisioned.session_id);
        return 8;
    }
    printf("offer head: %.120s...\n", offer.c_str());

    std::string answer;
    t0 = clock::now();
    const bool sdp_ok = session.exchangeSdpAnswer(
        provisioned.session_id,
        offer,
        answer,
        {},
        [](std::chrono::milliseconds d) { return sleepFor(d); });
    printf("exchangeSdpAnswer ok=%s elapsed_ms=%.0f answer_len=%zu api_err=%s\n",
           sdp_ok ? "yes" : "no",
           ms_since(t0),
           answer.size(),
           api->getLastError().c_str());
    if (!sdp_ok || answer.empty()) {
        api->deleteSession(provisioned.session_id);
        return 9;
    }
    printf("answer head: %.120s...\n", answer.c_str());
    peer.setRemoteAnswer(answer);

    auto local = peer.getLocalCandidates();
    printf("local ICE candidates=%zu\n", local.size());
    t0 = clock::now();
    const bool ice_send_ok =
        session.sendIceCandidates(
            provisioned.session_id,
            local,
            {},
            lunar::app::IceCandidateProcessor::usernameFragmentFromSdp(offer));
    printf("sendIceCandidates ok=%s elapsed_ms=%.0f err=%s\n",
           ice_send_ok ? "yes" : "no",
           ms_since(t0),
           api->getLastError().c_str());

    auto ice_profile =
        lunar::app::makeCloudStreamProfile(chosen_title_id, width, height);
    ice_profile.base_url = cloud.base_uri;
    ice_profile.msal_user_token = lpt;

    t0 = clock::now();
    auto remote = session.getIceCandidates(
        provisioned.session_id,
        ice_profile,
        {},
        [](std::chrono::milliseconds d) { return sleepFor(d); });
    printf("remote ICE count=%zu elapsed_ms=%.0f\n", remote.size(), ms_since(t0));
    for (const auto& c : remote) {
        peer.addIceCandidate(c.candidate);
    }

    for (int i = 0; i < 20; ++i) {
        peer.processEvents();
        if (peer.isConnected()) {
            printf("peer connected at tick %d\n", i);
            break;
        }
        sleepFor(std::chrono::milliseconds(500));
    }

    printf("summary: title=%s session=%s sdp=%s ice_send=%s remote_ice=%zu peer_connected=%s\n",
           chosen_title_id.c_str(),
           provisioned.session_id.c_str(),
           sdp_ok ? "ok" : "fail",
           ice_send_ok ? "ok" : "fail",
           remote.size(),
           peer.isConnected() ? "yes" : "no");

    api->sendKeepAlive(provisioned.session_id);
    const bool deleted = api->deleteSession(provisioned.session_id);
    printf("deleteSession ok=%s\n", deleted ? "yes" : "no");
    peer.disconnect();

    if (sdp_ok && !answer.empty()) {
        printf("=== HANDSHAKE PROTOCOL OK (SDP complete) ===\n");
        return 0;
    }
    printf("=== HANDSHAKE PROTOCOL FAIL ===\n");
    return 10;
}
