#include "../src/ps/psn_auth_utils.h"

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
    using lunar::ps::extractPsnAuthorizationCode;

    require(extractPsnAuthorizationCode(
                "https://remoteplay.dl.playstation.net/remoteplay/redirect?code=abc%2Bdef%2Fghi%3D&state=x") ==
                "abc+def/ghi=",
            "redirect URL code should be percent-decoded");
    require(extractPsnAuthorizationCode("  raw-code-value  ") == "raw-code-value",
            "raw authorization code should be accepted");
    require(extractPsnAuthorizationCode(
                "https://remoteplay.dl.playstation.net/remoteplay/redirect?error=access_denied").empty(),
            "redirect without code should be rejected");
    require(extractPsnAuthorizationCode("https://example.com/no-query").empty(),
            "arbitrary URL without code should be rejected");

    const uint8_t account_id[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    std::string encoded = lunar::ps::base64Encode(account_id, sizeof(account_id));
    std::string decoded;
    require(lunar::ps::base64Decode(encoded, decoded), "base64 account ID should decode");
    require(decoded == std::string(reinterpret_cast<const char*>(account_id), sizeof(account_id)),
            "base64 account ID should round-trip as raw bytes");

    std::cout << "PSN auth utility tests passed\n";
    return 0;
}
