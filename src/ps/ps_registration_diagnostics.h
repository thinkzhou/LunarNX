#pragma once

#include <cstdint>
#include <string>

namespace lunar::ps {

struct RegistrationDiagnostic {
    std::string stage;
    std::string detail;
    std::string progress;
    int http_code = -1;
    uint32_t application_reason = 0;
    bool has_application_reason = false;
};

RegistrationDiagnostic analyzeRegistrationLog(const char* log);
std::string formatRegistrationEvent(const char* outcome, int target,
                                    const char* log);

} // namespace lunar::ps
