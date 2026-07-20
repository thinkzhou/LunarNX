#include "../src/auth/xbox_auth_errors.h"
#include "../src/ui/qr_code.h"

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
    auto qr = lunar::ui::makeQrCode("https://www.microsoft.com/link");
    require(qr.size == 29, "Microsoft link QR should fit version 3");
    require(qr.at(0, 0), "top-left finder outer corner should be dark");
    require(qr.at(6, 0), "top-left finder outer edge should be dark");
    require(!qr.at(1, 1), "top-left finder separator ring should be light");
    require(qr.at(3, 3), "top-left finder center should be dark");
    require(qr.at(22, 22), "version 3 alignment center should be dark");
    require(!qr.modules.empty(), "QR modules should be present");

    std::string body =
        R"({"Identity":"0","XErr":2148916233,"Message":"","Redirect":"https://start.ui.xboxlive.com/CreateAccount"})";
    auto message = lunar::auth::describeXstsFailure(401, body);
    require(message.find("Xbox profile") != std::string::npos,
            "XErr 2148916233 should mention missing Xbox profile");
    require(message.find("start.ui.xboxlive.com/CreateAccount") != std::string::npos,
            "XErr 2148916233 should include Microsoft redirect URL");

    std::cout << "auth UI tests passed\n";
    return 0;
}
