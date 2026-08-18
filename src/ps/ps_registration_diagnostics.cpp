#include "ps_registration_diagnostics.h"

#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <vector>

namespace lunar::ps {
namespace {

bool contains(std::string_view messages, std::string_view needle) {
    return messages.find(needle) != std::string_view::npos;
}

long parseNumberAfter(std::string_view messages, std::string_view marker,
                      int base, bool* found) {
    *found = false;
    const size_t marker_pos = messages.find(marker);
    if (marker_pos == std::string_view::npos) return 0;

    const size_t value_pos = marker_pos + marker.size();
    std::string value(messages.substr(value_pos));
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, base);
    if (errno != 0 || end == value.c_str()) return 0;
    *found = true;
    return parsed;
}

std::string joinProgress(const std::vector<const char*>& progress) {
    if (progress.empty()) return "none";
    std::ostringstream out;
    for (size_t i = 0; i < progress.size(); ++i) {
        if (i) out << ',';
        out << progress[i];
    }
    return out.str();
}

} // namespace

RegistrationDiagnostic analyzeRegistrationLog(const char* log) {
    const std::string_view messages = log ? std::string_view(log) : std::string_view();
    RegistrationDiagnostic result;
    std::vector<const char*> progress;

    if (contains(messages, "Regist starting search"))
        progress.push_back("search-started");
    if (contains(messages, "Regist sending search packet"))
        progress.push_back("search-sent");
    if (contains(messages, "Regist received search response"))
        progress.push_back("search-response");
    if (contains(messages, "Regist connected to") &&
        contains(messages, "sending request"))
        progress.push_back("request-connected");
    if (contains(messages, "Regist waiting for response"))
        progress.push_back("request-sent");
    if (contains(messages, "Regist successfully received response"))
        progress.push_back("response-received");
    result.progress = joinProgress(progress);

    bool found_http = false;
    result.http_code = static_cast<int>(parseNumberAfter(
        messages, "Regist received HTTP code ", 10, &found_http));
    if (!found_http) result.http_code = -1;

    bool found_reason = false;
    const long reason = parseNumberAfter(
        messages, "Reported Application Reason: ", 0, &found_reason);
    if (found_reason) {
        result.has_application_reason = true;
        result.application_reason = static_cast<uint32_t>(reason);
    }

    if (contains(messages, "failed to generate random ambassador")) {
        result.stage = "crypto";
        result.detail = "Could not initialize registration encryption";
    } else if (contains(messages, "failed to format payload") ||
               contains(messages, "failed to format request")) {
        result.stage = "request-format";
        result.detail = "Could not prepare the registration request";
    } else if (contains(messages, "failed to getaddrinfo")) {
        result.stage = "invalid-address";
        result.detail = "Invalid console address";
    } else if (contains(messages, "failed to create socket for search")) {
        result.stage = "search-socket";
        result.detail = "Local pairing socket unavailable; restart LunarNX and try again";
    } else if (contains(messages, "failed to connect for search") ||
               contains(messages, "connect failed: tried all addresses")) {
        result.stage = "search-connect";
        result.detail = "Could not reach the console pairing port; check its IP address and network";
    } else if (contains(messages, "timed out waiting for search response") ||
               contains(messages, "Regist search failed")) {
        result.stage = "search-timeout";
        result.detail = "Console did not answer the pairing search; check pairing mode and console type";
    } else if (contains(messages, "failed to connect for request")) {
        result.stage = "request-connect";
        result.detail = "Could not connect to the console registration port";
    } else if (contains(messages, "failed to send request header") ||
               contains(messages, "failed to send payload")) {
        result.stage = "request-send";
        result.detail = "Could not send the registration request";
    } else if (found_http || found_reason) {
        result.stage = "console-rejected";
        result.detail = "Console rejected registration; check the active user's Account ID and PIN";
    } else if (contains(messages, "failed to receive response HTTP header")) {
        result.stage = "response-timeout";
        result.detail = "Timed out waiting for the console registration response";
    } else if (contains(messages, "response does not contain") ||
               contains(messages, "failed to pare response") ||
               contains(messages, "failed to parse response")) {
        result.stage = "invalid-response";
        result.detail = "Console returned an invalid registration response";
    } else if (contains(messages, "Regist successfully received response")) {
        result.stage = "success";
    } else {
        result.stage = "unknown";
        result.detail = "Registration failed; check the active user's Account ID, PIN, and console type";
    }

    return result;
}

std::string formatRegistrationEvent(const char* outcome, int target,
                                    const char* log) {
    std::ostringstream out;
    out << (outcome ? outcome : "unknown")
        << " target=" << target << '\n';
    if (log) out << log;
    return out.str();
}

} // namespace lunar::ps
