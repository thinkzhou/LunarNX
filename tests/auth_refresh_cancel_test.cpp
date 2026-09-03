#include "api/http_client.h"
#include "auth/auth_manager.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

bool require(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

} // namespace

int main() {
    char token_path[] = "/tmp/lunarnx-auth-cancel-XXXXXX";
    const int token_fd = mkstemp(token_path);
    if (!require(token_fd >= 0, "create temporary token file")) return 1;
    close(token_fd);

    {
        std::ofstream token_file(token_path, std::ios::trunc);
        token_file << R"({
            "access_token": "expired-access-token",
            "refresh_token": "refresh-token",
            "expires_at_ms": 1
        })";
    }

    lunar::api::HttpClient http;
    lunar::auth::AuthManager auth(http);
    const bool loaded = auth.loadTokens(token_path);
    std::remove(token_path);
    if (!require(loaded, "load refreshable token fixture")) return 1;

    int first_cancel_checks = 0;
    const bool first_result = auth.refreshTokensIfNeeded([&]() {
        return ++first_cancel_checks >= 2;
    });

    int retry_cancel_checks = 0;
    const bool retry_result = auth.refreshTokensIfNeeded([&]() {
        return ++retry_cancel_checks >= 2;
    });

    bool ok = true;
    ok &= require(!first_result, "cancelled refresh should fail quietly");
    ok &= require(first_cancel_checks >= 2,
                  "first refresh should reach cancellable HTTP acquisition");
    ok &= require(!retry_result, "cancelled retry should fail quietly");
    ok &= require(retry_cancel_checks >= 2,
                  "cancellation must not throttle an immediate refresh retry");
    if (!ok) return 1;

    std::cout << "Auth refresh cancellation tests passed\n";
    return 0;
}
