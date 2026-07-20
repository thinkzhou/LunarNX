/**
 * LunarNX Desktop Auth Test
 *
 * Validates the Xbox authentication + API pipeline on macOS without WebRTC.
 * Tests: MSAL Device Code Flow → RPS → XSTS → GSSV → Console list.
 *
 * Build: make -f Makefile.desktop auth_test
 * Run:   ./build/pc/lunar_auth_test
 */

#include "../auth/auth_manager.h"
#include "../api/xbox_api_client.h"
#include <cstdio>

int main() {
    printf("=== LunarNX Auth Test ===\n\n");

    lunar::api::HttpClient http;
    lunar::auth::AuthManager auth(http);

    // Step 1: Start device code auth
    printf("Starting Device Code Flow...\n");
    if (!auth.startDeviceCodeAuth()) {
        fprintf(stderr, "FAIL: Device code request failed.\n");
        return 1;
    }

    printf("\n  URL:  %s\n", auth.getVerificationUri().c_str());
    printf("  Code: %s\n\n", auth.getUserCode().c_str());
    printf("Open the URL in a browser, enter the code, and sign in.\n");
    printf("Press Enter after completing the login...\n");
    getchar();

    // Step 2: Poll for token
    printf("Polling for token...\n");
    if (!auth.pollForToken()) {
        fprintf(stderr, "FAIL: Token polling failed.\n");
        return 1;
    }

    printf("PASS: Authenticated as '%s'\n", auth.getGamertag().c_str());
    printf("  Web token: %s...\n", auth.getWebToken().substr(0, 40).c_str());
    printf("  User hash: %s\n", auth.getUserHash().c_str());

    // Step 3: Fetch console list
    printf("\nFetching console list...\n");
    std::printf("[auth] home gsToken: %s\n", auth.getGssvToken().empty() ? "(empty)" : "(ok)");
    std::printf("[auth] cloud access: %s base=%s\n",
                auth.hasCloudAccess() ? "yes" : "no",
                auth.getCloudStreamingToken().base_uri.c_str());
    lunar::api::XboxApiClient api(http, auth.getWebToken(), auth.getUserHash(), auth.getGssvToken());
    if (!auth.getHomeStreamingToken().base_uri.empty()) {
        api.setBaseUrl(auth.getHomeStreamingToken().base_uri);
    }

    auto consoles = api.getConsoles();
    if (consoles.empty()) {
        fprintf(stderr, "FAIL: No consoles found.\n");
        return 1;
    }

    printf("PASS: Found %zu console(s):\n", consoles.size());
    for (auto& c : consoles) {
        printf("  - %s (server_id: %s)\n", c.name.c_str(), c.id.c_str());
    }

    // Step 4: Create a session (test connectivity, then delete)
    printf("\nCreating test session on '%s'...\n", consoles[0].name.c_str());
    std::string sid = api.createSession(consoles[0].id);
    if (sid.empty()) {
        fprintf(stderr, "FAIL: Session creation failed.\n");
        return 1;
    }
    printf("PASS: Session created: %s\n", sid.c_str());

    // Cleanup
    api.deleteSession(sid);
    printf("Session deleted.\n");

    // Step 5: Save tokens for future use
    auth.saveTokens("./token_test.json");
    printf("Tokens saved to ./token_test.json\n");

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
