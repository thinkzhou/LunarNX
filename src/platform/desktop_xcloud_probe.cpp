// Desktop-only xCloud network/protocol probe.
// Usage:
//   ./build/pc/xcloud_probe
// Optional env:
//   LUNARNX_TOKEN_PATH=./token.json
//   LUNARNX_FORCE_REGION_IP=4.2.2.2
//   LUNARNX_CURL_TIMEOUT_MS is compile-time; rebuild with -DLUNARNX_CURL_TIMEOUT_MS=120000

#include "../api/http_client.h"
#include "../api/xbox_api_client.h"
#include "../auth/auth_manager.h"
#include "../common.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using clock = std::chrono::steady_clock;

double ms_since(clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

} // namespace

int main() {
    const char* token_path = getenv("LUNARNX_TOKEN_PATH");
    if (!token_path || !token_path[0]) token_path = lunar::get_token_path();
    const char* region = getenv("LUNARNX_FORCE_REGION_IP");
    if (!region) region = "4.2.2.2";

    printf("=== LunarNX xCloud probe (desktop) ===\n");
    printf("token_path=%s\n", token_path);
    printf("force_region_ip=%s\n", region[0] ? region : "(empty)");

    lunar::api::HttpClient http;
    lunar::auth::AuthManager auth(http);
    auth.setForceRegionIp(region);

    if (!auth.loadTokens(token_path)) {
        fprintf(stderr, "FAIL: loadTokens(%s)\n", token_path);
        fprintf(stderr, "Run desktop auth first or copy Switch token.json.\n");
        return 1;
    }
    printf("loaded saved session gamertag=%s cloud_before=%s\n",
           auth.getGamertag().c_str(),
           auth.hasCloudAccess() ? "yes" : "no");

    auto t0 = clock::now();
    const bool refreshed = auth.refreshStreamingTokens(true);
    printf("refreshStreamingTokens force=true ok=%s elapsed_ms=%.0f cloud=%s home=%s err=%s\n",
           refreshed ? "yes" : "no",
           ms_since(t0),
           auth.hasCloudAccess() ? "yes" : "no",
           auth.getHomeStreamingToken().valid() ? "yes" : "no",
           auth.getLastError().c_str());
    auth.saveTokens(token_path);

    if (!auth.hasCloudAccess()) {
        fprintf(stderr, "FAIL: no cloud token. If InvalidCountry, set LUNARNX_FORCE_REGION_IP.\n");
        return 2;
    }

    auto cloud = auth.getCloudStreamingToken();
    printf("cloud_base=%s duration=%d token_len=%zu\n",
           cloud.base_uri.c_str(),
           cloud.duration_seconds,
           cloud.gs_token.size());

    lunar::api::XboxApiClient api(http, auth.getWebToken(), auth.getUserHash(), cloud.gs_token);
    api.setSessionKind(lunar::api::GssvSessionKind::Cloud);
    if (!cloud.base_uri.empty()) api.setBaseUrl(cloud.base_uri);

    // Probe recent first (small)
    t0 = clock::now();
    auto recent = api.getRecentCloudTitles();
    printf("getRecentCloudTitles count=%zu elapsed_ms=%.0f err=%s\n",
           recent.size(), ms_since(t0), api.getLastError().c_str());
    for (size_t i = 0; i < recent.size() && i < 5; ++i) {
        printf("  recent[%zu] %s product=%s\n",
               i,
               recent[i].name.empty() ? recent[i].title_id.c_str() : recent[i].name.c_str(),
               recent[i].product_id.c_str());
    }

    // Probe full titles (may stall)
    t0 = clock::now();
    auto full = api.getCloudTitles();
    printf("getCloudTitles count=%zu elapsed_ms=%.0f err=%s\n",
           full.size(), ms_since(t0), api.getLastError().c_str());

    // Hydrated library path used by UI
    t0 = clock::now();
    std::vector<lunar::api::CloudTitle> recent_out, new_out;
    auto library = api.getHydratedCloudLibrary(&recent_out, &new_out);
    printf("getHydratedCloudLibrary library=%zu recent=%zu new=%zu elapsed_ms=%.0f err=%s\n",
           library.size(), recent_out.size(), new_out.size(),
           ms_since(t0), api.getLastError().c_str());

    size_t with_poster = 0;
    for (const auto& t : library) if (!t.image_url.empty()) ++with_poster;
    printf("library_with_poster=%zu / %zu\n", with_poster, library.size());
    for (size_t i = 0; i < library.size() && i < 8; ++i) {
        printf("  lib[%zu] %s poster=%s\n",
               i,
               library[i].name.empty() ? library[i].title_id.c_str() : library[i].name.c_str(),
               library[i].image_url.empty() ? "(none)" : library[i].image_url.c_str());
    }

    printf("=== probe done ===\n");
    return library.empty() ? 3 : 0;
}
