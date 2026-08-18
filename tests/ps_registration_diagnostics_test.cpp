#include "../src/ps/ps_registration_diagnostics.h"

#include <cassert>
#include <string>

int main() {
    const char* rejected_log =
        "Regist starting search\n"
        "Regist sending search packet\n"
        "Regist received search response from 192.168.1.38\n"
        "Regist connected to 192.168.1.38, sending request\n"
        "Regist waiting for response\n"
        "Regist received HTTP code 403\n"
        "Reported Application Reason: 0x80108b11 (invalid pin)\n"
        "secret-account-id AQIDBAUGBwg=\n"
        "secret-pin 12345678\n";

    const auto rejected = lunar::ps::analyzeRegistrationLog(rejected_log);
    assert(rejected.stage == "console-rejected");
    assert(rejected.progress ==
           "search-started,search-sent,search-response,request-connected,request-sent");
    assert(rejected.http_code == 403);
    assert(rejected.has_application_reason);
    assert(rejected.application_reason == 0x80108b11u);

    const std::string event =
        lunar::ps::formatRegistrationEvent("failed", 1000, rejected_log);
    assert(event == std::string("failed target=1000\n") + rejected_log);
    assert(event.find("failed target=1000") != std::string::npos);
    assert(event.find("target=1000") != std::string::npos);
    assert(event.find("Regist received HTTP code 403") != std::string::npos);
    assert(event.find("Reported Application Reason: 0x80108b11 (invalid pin)") !=
           std::string::npos);
    assert(event.find("AQIDBAUGBwg=") != std::string::npos);
    assert(event.find("12345678") != std::string::npos);
    assert(event.find("192.168.1.38") != std::string::npos);

    return 0;
}
