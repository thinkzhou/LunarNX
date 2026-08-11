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
    require(qr.size >= 21, "Microsoft link QR should use a valid QR version");
    require(qr.at(0, 0), "top-left finder outer corner should be dark");
    require(qr.at(6, 0), "top-left finder outer edge should be dark");
    require(!qr.at(1, 1), "top-left finder separator ring should be light");
    require(qr.at(3, 3), "top-left finder center should be dark");
    require(!qr.modules.empty(), "QR modules should be present");

    const std::string psn_url =
        "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize"
        "?service_entity=urn%3Aservice-entity%3Apsn&response_type=code"
        "&client_id=ba495a24-818c-472b-b12d-ff231c1b5745"
        "&redirect_uri=https%3A%2F%2Fremoteplay.dl.playstation.net%2Fremoteplay%2Fredirect"
        "&scope=psn%3Aclientapp%20referenceDataService%3AcountryConfig.read%20"
        "pushNotification%3AwebSocket.desktop.connect%20"
        "sessionManager%3AremotePlaySession.system.update"
        "&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup"
        "&smcid=remoteplay&prompt=always&PlatformPrivacyWs1=minimal"
        "&duid=00000007004100800123456789abcdef0123456789abcdef";
    auto psn_qr = lunar::ui::makeQrCode(psn_url);
    require(!psn_qr.empty(), "full Sony OAuth URL should produce a QR code");
    require(psn_qr.size <= 97, "Sony OAuth QR should remain scannable at 260px");

    auto psn_bitmap = lunar::ui::makeQrBitmap(psn_qr);
    require(psn_bitmap.size == psn_qr.size + 8,
            "QR bitmap should include a four-module quiet zone");
    require(psn_bitmap.rgba.size() ==
                static_cast<size_t>(psn_bitmap.size * psn_bitmap.size * 4),
            "QR bitmap should contain one RGBA pixel per module");
    require(psn_bitmap.rgba[0] == 0xFF && psn_bitmap.rgba[1] == 0xFF &&
                psn_bitmap.rgba[2] == 0xFF && psn_bitmap.rgba[3] == 0xFF,
            "QR quiet zone should be opaque white");
    const size_t first_module = static_cast<size_t>((4 * psn_bitmap.size + 4) * 4);
    require(psn_bitmap.rgba[first_module] == 0 &&
                psn_bitmap.rgba[first_module + 1] == 0 &&
                psn_bitmap.rgba[first_module + 2] == 0 &&
                psn_bitmap.rgba[first_module + 3] == 0xFF,
            "dark QR modules should be opaque black");

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
