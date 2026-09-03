#include "ice_candidate_processor.h"

#include <cJSON.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace lunar::app {

namespace {

std::string trim(std::string value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool candidateIsInvalid(const std::string& candidate) {
    const std::string lowered = lower(candidate);
    return lowered.find(" udp ") != std::string::npos &&
           lowered.find("tcptype") != std::string::npos;
}

std::string normalizeForLibPeer(std::string candidate) {
    candidate = trim(std::move(candidate));
    if (startsWith(candidate, "candidate:")) {
        return "a=" + candidate;
    }
    if (candidate == "end-of-candidates") {
        return "a=end-of-candidates";
    }
    return candidate;
}

std::string normalizeForApi(std::string candidate) {
    candidate = trim(std::move(candidate));
    if (startsWith(candidate, "a=candidate:")) {
        return candidate.substr(2);
    }
    if (candidate == "end-of-candidates") {
        return "a=end-of-candidates";
    }
    return candidate;
}

bool isAcceptedCandidate(const std::string& candidate) {
    return startsWith(candidate, "a=candidate:") ||
           startsWith(candidate, "candidate:") ||
           candidate == "a=end-of-candidates" ||
           candidate == "end-of-candidates";
}

bool hasSameCandidate(const std::vector<IceCandidatePayload>& candidates,
                      const IceCandidatePayload& payload) {
    return std::find_if(
               candidates.begin(),
               candidates.end(),
               [&payload](const IceCandidatePayload& existing) {
                   return existing.candidate == payload.candidate &&
                          existing.sdp_mid == payload.sdp_mid &&
                          existing.sdp_mline_index == payload.sdp_mline_index;
               }) != candidates.end();
}

void appendCandidate(IceCandidatePayload payload,
                     std::vector<IceCandidatePayload>& candidates) {
    payload.candidate = normalizeForLibPeer(payload.candidate);
    if (payload.candidate.empty() || !isAcceptedCandidate(payload.candidate)) {
        return;
    }
    if (candidateIsInvalid(payload.candidate)) {
        return;
    }

    if (!hasSameCandidate(candidates, payload)) {
        candidates.push_back(std::move(payload));
    }
}

struct CandidateParts {
    std::string foundation;
    int component = 0;
    std::string protocol;
    uint32_t priority = 0;
    std::string ip;
    int port = 0;
    std::string type;
    std::string remainder;
};

bool parseCandidateParts(const std::string& candidate, CandidateParts& parts) {
    std::string line = normalizeForLibPeer(candidate);
    if (!startsWith(line, "a=candidate:")) {
        return false;
    }
    line = line.substr(std::string("a=candidate:").size());

    std::istringstream stream(line);
    stream >> parts.foundation
           >> parts.component
           >> parts.protocol
           >> parts.priority
           >> parts.ip
           >> parts.port;
    if (!stream || parts.port <= 0) {
        return false;
    }

    std::getline(stream, parts.remainder);
    parts.remainder = trim(std::move(parts.remainder));
    std::istringstream remainder(parts.remainder);
    std::string typ_token;
    remainder >> typ_token >> parts.type;
    return remainder && typ_token == "typ";
}

bool decodeTeredoAddress(const std::string& ip,
                         std::string& client_ipv4,
                         int& mapped_port) {
    std::array<uint8_t, 16> address{};
    if (inet_pton(AF_INET6, ip.c_str(), address.data()) != 1) {
        return false;
    }

    if (address[0] != 0x20 || address[1] != 0x01 ||
        address[2] != 0x00 || address[3] != 0x00) {
        return false;
    }

    const uint16_t obfuscated_port =
        (static_cast<uint16_t>(address[10]) << 8) |
        static_cast<uint16_t>(address[11]);
    mapped_port = static_cast<int>((~obfuscated_port) & 0xffff);
    if (mapped_port <= 0) {
        return false;
    }

    char out[16] = {};
    std::snprintf(out,
                  sizeof(out),
                  "%u.%u.%u.%u",
                  static_cast<unsigned>(address[12] ^ 0xff),
                  static_cast<unsigned>(address[13] ^ 0xff),
                  static_cast<unsigned>(address[14] ^ 0xff),
                  static_cast<unsigned>(address[15] ^ 0xff));
    client_ipv4 = out;
    return true;
}

IceCandidatePayload makeDerivedTeredoCandidate(const IceCandidatePayload& source,
                                               const char* foundation,
                                               uint32_t priority,
                                               const std::string& client_ipv4,
                                               int port) {
    IceCandidatePayload derived = source;
    std::ostringstream candidate;
    candidate << "a=candidate:" << foundation
              << " 1 UDP " << priority
              << " " << client_ipv4
              << " " << port
              << " typ host";
    derived.candidate = candidate.str();
    derived.sdp_mid = source.sdp_mid.empty() ? "0" : source.sdp_mid;
    derived.sdp_mline_index = source.sdp_mline_index;
    derived.message_type = source.message_type.empty()
        ? "iceCandidate"
        : source.message_type;
    return derived;
}

std::vector<IceCandidatePayload> expandTeredoCandidatesLikeXStreaming(
    std::vector<IceCandidatePayload> candidates) {
    std::vector<IceCandidatePayload> expanded;
    expanded.reserve(candidates.size() + 2);
    for (auto& payload : candidates) {
        CandidateParts parts;
        if (payload.candidate == "a=end-of-candidates" ||
            !parseCandidateParts(payload.candidate, parts)) {
            appendCandidate(std::move(payload), expanded);
            continue;
        }

        std::string client_ipv4;
        int mapped_port = 0;
        if (decodeTeredoAddress(parts.ip, client_ipv4, mapped_port)) {
            IceCandidatePayload xbox_default = makeDerivedTeredoCandidate(
                payload, "10", 1u, client_ipv4, 9002);
            appendCandidate(std::move(xbox_default), expanded);

            IceCandidatePayload mapped = makeDerivedTeredoCandidate(
                payload, "11", 1u, client_ipv4, mapped_port);
            appendCandidate(std::move(mapped), expanded);
        }
        appendCandidate(std::move(payload), expanded);
    }
    return expanded;
}

bool parseIpv4Octets(const std::string& ip, std::array<unsigned int, 4>& octets) {
    char trailing = '\0';
    const int parsed = std::sscanf(ip.c_str(),
                                   "%u.%u.%u.%u%c",
                                   &octets[0],
                                   &octets[1],
                                   &octets[2],
                                   &octets[3],
                                   &trailing);
    if (parsed != 4) {
        return false;
    }
    return std::all_of(octets.begin(), octets.end(), [](unsigned int value) {
        return value <= 255;
    });
}

bool isPrivateIpv4(const std::string& ip) {
    std::array<unsigned int, 4> octets{};
    if (!parseIpv4Octets(ip, octets)) {
        return false;
    }
    return octets[0] == 10 ||
           (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
           (octets[0] == 192 && octets[1] == 168);
}

bool isPublicIpv4(const std::string& ip) {
    std::array<unsigned int, 4> octets{};
    if (!parseIpv4Octets(ip, octets)) {
        return false;
    }
    return !isPrivateIpv4(ip) &&
           octets[0] != 0 &&
           octets[0] != 127 &&
           octets[0] < 224;
}

bool addressesEqual(const std::string& left, const std::string& right) {
    if (left == right) {
        return true;
    }
    const bool left_v6 = left.find(':') != std::string::npos;
    const bool right_v6 = right.find(':') != std::string::npos;
    if (left_v6 != right_v6) {
        return false;
    }

    std::array<uint8_t, 16> left_bytes{};
    std::array<uint8_t, 16> right_bytes{};
    const int family = left_v6 ? AF_INET6 : AF_INET;
    const size_t byte_count = left_v6 ? 16 : 4;
    return inet_pton(family, left.c_str(), left_bytes.data()) == 1 &&
           inet_pton(family, right.c_str(), right_bytes.data()) == 1 &&
           std::memcmp(left_bytes.data(), right_bytes.data(), byte_count) == 0;
}

// Home: prefer LAN private IPv4 first, then public IPv4, then IPv6.
// Cloud/WAN: prefer public IPv4 / IPv6 and deprioritize private LAN candidates.
int remoteCandidateRank(const CandidateParts& parts, bool prefer_public) {
    const bool is_v6 = parts.ip.find(':') != std::string::npos;
    if (prefer_public) {
        if (isPublicIpv4(parts.ip)) {
            return 0;
        }
        if (is_v6) {
            return 1;
        }
        if (isPrivateIpv4(parts.ip)) {
            return 2;
        }
        return 3;
    }

    if (isPrivateIpv4(parts.ip)) {
        return 0;
    }
    if (isPublicIpv4(parts.ip)) {
        return 1;
    }
    if (is_v6) {
        return 2;
    }
    return 3;
}

std::vector<IceCandidatePayload> rewriteCandidatesLikeXStreaming(
    std::vector<IceCandidatePayload> candidates,
    bool prefer_public,
    const std::string& preferred_address,
    int preferred_port) {
    auto expanded = expandTeredoCandidatesLikeXStreaming(std::move(candidates));

    struct RankedCandidate {
        IceCandidatePayload payload;
        CandidateParts parts;
        bool preferred = false;
        int rank = 3;
        size_t order = 0;
    };

    std::vector<RankedCandidate> ranked;
    ranked.reserve(expanded.size());
    IceCandidatePayload end_marker;
    end_marker.candidate = "a=end-of-candidates";
    end_marker.message_type = "iceCandidate";
    size_t order = 0;
    for (auto& candidate : expanded) {
        if (candidate.candidate == "a=end-of-candidates") {
            end_marker = std::move(candidate);
            continue;
        }

        CandidateParts parts;
        if (!parseCandidateParts(candidate.candidate, parts)) {
            continue;
        }
        const int rank = remoteCandidateRank(parts, prefer_public);
        const bool preferred = !prefer_public && preferred_port > 0 &&
            addressesEqual(parts.ip, preferred_address) &&
            parts.port == preferred_port;
        ranked.push_back(RankedCandidate{
            std::move(candidate),
            std::move(parts),
            preferred,
            rank,
            order++,
        });
    }

    std::stable_sort(ranked.begin(),
                     ranked.end(),
                     [](const RankedCandidate& left, const RankedCandidate& right) {
                         if (left.preferred != right.preferred) {
                             return left.preferred;
                         }
                         if (left.rank != right.rank) {
                             return left.rank < right.rank;
                         }
                         return left.order < right.order;
                     });

    std::vector<IceCandidatePayload> rewritten;
    rewritten.reserve(ranked.size() + 1);
    unsigned int foundation = 1;
    for (auto& candidate : ranked) {
        std::ostringstream line;
        line << "a=candidate:" << foundation
             << " 1 UDP " << (foundation == 1 ? 2130706431u : 1u)
             << " " << candidate.parts.ip
             << " " << candidate.parts.port
             << " " << candidate.parts.remainder;
        candidate.payload.candidate = line.str();
        candidate.payload.sdp_mid = candidate.payload.sdp_mid.empty()
            ? "0"
            : candidate.payload.sdp_mid;
        candidate.payload.message_type = candidate.payload.message_type.empty()
            ? "iceCandidate"
            : candidate.payload.message_type;
        appendCandidate(std::move(candidate.payload), rewritten);
        ++foundation;
    }
    appendCandidate(std::move(end_marker), rewritten);
    return rewritten;
}

void appendCandidateFromJson(cJSON* item,
                             std::vector<IceCandidatePayload>& candidates) {
    if (!item) {
        return;
    }

    IceCandidatePayload payload;
    if (cJSON_IsString(item) && item->valuestring) {
        cJSON* nested = cJSON_Parse(item->valuestring);
        if (nested) {
            appendCandidateFromJson(nested, candidates);
            cJSON_Delete(nested);
            return;
        }
        payload.candidate = item->valuestring;
        appendCandidate(std::move(payload), candidates);
        return;
    }

    if (!cJSON_IsObject(item)) {
        return;
    }

    cJSON* candidate = cJSON_GetObjectItem(item, "candidate");
    if (!candidate || !cJSON_IsString(candidate) || !candidate->valuestring) {
        return;
    }
    payload.candidate = candidate->valuestring;

    cJSON* sdp_mid = cJSON_GetObjectItem(item, "sdpMid");
    if (sdp_mid && cJSON_IsString(sdp_mid) && sdp_mid->valuestring) {
        payload.sdp_mid = sdp_mid->valuestring;
    }

    cJSON* sdp_mline_index = cJSON_GetObjectItem(item, "sdpMLineIndex");
    if (sdp_mline_index && cJSON_IsNumber(sdp_mline_index)) {
        payload.sdp_mline_index = sdp_mline_index->valueint;
    }

    cJSON* message_type = cJSON_GetObjectItem(item, "messageType");
    if (message_type && cJSON_IsString(message_type) && message_type->valuestring) {
        payload.message_type = message_type->valuestring;
    }

    appendCandidate(std::move(payload), candidates);
}

void appendCandidatesFromJsonRoot(cJSON* root,
                                  std::vector<IceCandidatePayload>& candidates) {
    if (!root) {
        return;
    }

    if (cJSON_IsArray(root)) {
        const int count = cJSON_GetArraySize(root);
        for (int i = 0; i < count; ++i) {
            appendCandidateFromJson(cJSON_GetArrayItem(root, i), candidates);
        }
        return;
    }

    if (!cJSON_IsObject(root)) {
        appendCandidateFromJson(root, candidates);
        return;
    }

    cJSON* exchange_response = cJSON_GetObjectItem(root, "exchangeResponse");
    if (exchange_response && cJSON_IsString(exchange_response) &&
        exchange_response->valuestring) {
        cJSON* inner = cJSON_Parse(exchange_response->valuestring);
        if (inner) {
            appendCandidatesFromJsonRoot(inner, candidates);
            cJSON_Delete(inner);
            return;
        }
    }

    cJSON* list = cJSON_GetObjectItem(root, "iceCandidates");
    if (!list) {
        list = cJSON_GetObjectItem(root, "candidates");
    }
    if (!list) {
        list = cJSON_GetObjectItem(root, "candidate");
    }

    if (list && cJSON_IsArray(list)) {
        const int count = cJSON_GetArraySize(list);
        for (int i = 0; i < count; ++i) {
            appendCandidateFromJson(cJSON_GetArrayItem(list, i), candidates);
        }
    } else {
        appendCandidateFromJson(root, candidates);
    }
}

void appendRawCandidates(const std::string& payload,
                         std::vector<IceCandidatePayload>& candidates) {
    std::istringstream lines(payload);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(std::move(line));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        IceCandidatePayload candidate;
        candidate.candidate = std::move(line);
        appendCandidate(std::move(candidate), candidates);
    }
}

} // namespace

std::string IceCandidateProcessor::usernameFragmentFromSdp(
    const std::string& sdp) {
    const std::string marker = "a=ice-ufrag:";
    const auto at = sdp.find(marker);
    if (at == std::string::npos) {
        return {};
    }
    const auto start = at + marker.size();
    const auto end = sdp.find_first_of("\r\n", start);
    std::string value = sdp.substr(start,
                                   end == std::string::npos ? std::string::npos
                                                             : end - start);
    return trim(std::move(value));
}

std::vector<IceCandidatePayload> IceCandidateProcessor::fromLocal(
    const std::vector<webrtc::IceCandidate>& local,
    const std::string& username_fragment) const {
    std::vector<IceCandidatePayload> payloads;
    payloads.reserve(local.size() + 1);
    for (const auto& candidate : local) {
        IceCandidatePayload payload;
        payload.candidate = normalizeForApi(candidate.sdp);
        payload.sdp_mid = candidate.sdp_mid.empty() ? "0" : candidate.sdp_mid;
        payload.sdp_mline_index = candidate.sdp_mline_index;
        payload.username_fragment = username_fragment;
        if (!payload.candidate.empty() && !candidateIsInvalid(payload.candidate)) {
            payloads.push_back(std::move(payload));
        }
    }
    IceCandidatePayload end_marker;
    end_marker.candidate = "a=end-of-candidates";
    end_marker.username_fragment = username_fragment;
    payloads.push_back(std::move(end_marker));
    return payloads;
}

std::vector<IceCandidatePayload> IceCandidateProcessor::parseRemotePayload(
    const std::string& payload,
    const StreamProfile& profile) const {
    return parseRemotePayloads({payload}, profile);
}

std::vector<IceCandidatePayload> IceCandidateProcessor::parseRemotePayloads(
    const std::vector<std::string>& payloads,
    const StreamProfile& profile) const {
    if (payloads.empty()) {
        return {};
    }
    std::vector<IceCandidatePayload> candidates;
    for (const auto& payload : payloads) {
        const size_t before = candidates.size();
        cJSON* root = cJSON_Parse(payload.c_str());
        const bool parsed_json = root != nullptr;
        if (root) {
            appendCandidatesFromJsonRoot(root, candidates);
            cJSON_Delete(root);
        }
        if (!parsed_json || candidates.size() == before) {
            appendRawCandidates(payload, candidates);
        }
    }

    const bool prefer_public = profile.type == SessionType::Cloud || profile.prefer_ipv6;
    return rewriteCandidatesLikeXStreaming(
        std::move(candidates),
        prefer_public,
        profile.preferred_remote_ice_address,
        profile.preferred_remote_ice_port);
}

bool IceCandidateProcessor::hasRealCandidate(const std::string& payload) const {
    std::vector<IceCandidatePayload> candidates;
    const size_t before = candidates.size();
    cJSON* root = cJSON_Parse(payload.c_str());
    const bool parsed_json = root != nullptr;
    if (root) {
        appendCandidatesFromJsonRoot(root, candidates);
        cJSON_Delete(root);
    }
    if (!parsed_json || candidates.size() == before) {
        appendRawCandidates(payload, candidates);
    }

    for (const auto& candidate : candidates) {
        CandidateParts parts;
        if (parseCandidateParts(candidate.candidate, parts) && parts.priority > 1000) {
            return true;
        }
    }
    return false;
}

std::string IceCandidateProcessor::toApiJson(
    const std::vector<IceCandidatePayload>& candidates) const {
    cJSON* root = cJSON_CreateObject();
    cJSON* list = cJSON_CreateArray();

    for (const auto& candidate : candidates) {
        const std::string api_candidate = normalizeForApi(candidate.candidate);
        if (api_candidate.empty() || candidateIsInvalid(api_candidate)) {
            continue;
        }

        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "candidate", api_candidate.c_str());
        cJSON_AddStringToObject(item, "sdpMid", candidate.sdp_mid.c_str());
        cJSON_AddNumberToObject(item, "sdpMLineIndex", candidate.sdp_mline_index);
        if (!candidate.username_fragment.empty()) {
            cJSON_AddStringToObject(item, "usernameFragment",
                                    candidate.username_fragment.c_str());
        }
        char* serialized = cJSON_PrintUnformatted(item);
        if (serialized) {
            cJSON_AddItemToArray(list, cJSON_CreateString(serialized));
            std::free(serialized);
        }
        cJSON_Delete(item);
    }

    cJSON_AddStringToObject(root, "messageType", "iceCandidate");
    cJSON_AddItemToObject(root, "candidate", list);
    char* printed = cJSON_PrintUnformatted(root);
    std::string json = printed ? printed : R"({"messageType":"iceCandidate","candidate":[]})";
    if (printed) {
        std::free(printed);
    }
    cJSON_Delete(root);
    return json;
}

std::vector<std::string> IceCandidateProcessor::toLibPeerLines(
    const std::vector<IceCandidatePayload>& candidates) const {
    std::vector<std::string> lines;
    lines.reserve(candidates.size());

    for (const auto& candidate : candidates) {
        std::string line = normalizeForLibPeer(candidate.candidate);
        if (line == "a=end-of-candidates" || candidateIsInvalid(line)) {
            continue;
        }
        if (startsWith(line, "a=candidate:")) {
            lines.push_back(std::move(line));
        }
    }

    return lines;
}

} // namespace lunar::app
