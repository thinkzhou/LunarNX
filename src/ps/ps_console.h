#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lunar::ps {

enum class PsConsoleType { Unknown, PS4, PS5 };
enum class PsConsoleState { Unknown, Ready, Standby };
enum class PsRemoteEndpointKind { MainPS4, DevicePS5 };

constexpr int kPs5TargetThreshold = 1000000;
constexpr int kPs4RemoteTarget = 1000;
constexpr int kPs5RemoteTarget = 1000100;

inline bool isPs5Target(int target) {
    return target >= kPs5TargetThreshold;
}

inline std::optional<std::string> normalizeHexId(std::string_view text, size_t bytes) {
    std::string normalized;
    normalized.reserve(bytes * 2);
    for (unsigned char c : text) {
        if (c == ':' || c == '-') continue;
        if (!std::isxdigit(c)) return std::nullopt;
        normalized.push_back(static_cast<char>(std::tolower(c)));
    }
    if (normalized.size() != bytes * 2) return std::nullopt;
    return normalized;
}

inline std::optional<std::string> normalizeMac(std::string_view text) {
    return normalizeHexId(text, 6);
}

inline std::optional<std::string> normalizeDuid(std::string_view text) {
    return normalizeHexId(text, 32);
}

inline std::string macFromBytes(const uint8_t bytes[6]) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string value(12, '0');
    for (size_t i = 0; i < 6; ++i) {
        value[i * 2] = kHex[bytes[i] >> 4];
        value[i * 2 + 1] = kHex[bytes[i] & 0xf];
    }
    return value;
}

inline bool decodeDuid(std::string_view text, uint8_t out[32]) {
    auto normalized = normalizeDuid(text);
    if (!normalized) return false;
    auto nibble = [](char c) -> uint8_t {
        return c <= '9' ? static_cast<uint8_t>(c - '0')
                        : static_cast<uint8_t>(c - 'a' + 10);
    };
    for (size_t i = 0; i < 32; ++i) {
        out[i] = static_cast<uint8_t>((nibble((*normalized)[i * 2]) << 4) |
                                      nibble((*normalized)[i * 2 + 1]));
    }
    return true;
}

struct PsLocalEndpoint {
    std::string ip;
    uint16_t port = 0;
    PsConsoleState state = PsConsoleState::Unknown;
    // True only when this endpoint was discovered or paired during the
    // current process. A persisted last-known address is display metadata,
    // not proof that the console is reachable now.
    bool verified = false;
};

struct PsRemoteEndpoint {
    PsRemoteEndpointKind kind = PsRemoteEndpointKind::DevicePS5;
    std::string duid;
    std::string device_name;
    bool remoteplay_enabled = false;
};

struct RegisteredCredential {
    std::string server_mac;
    std::string nickname;
    std::string last_known_addr;
    std::string psn_duid;
    int target = 0;
    uint8_t rp_regist_key[0x10] = {};
    uint32_t rp_key_type = 0;
    uint8_t rp_key[0x10] = {};
    std::string console_login_pin;
};

inline bool hasRegisteredPs4(
    const std::vector<RegisteredCredential>& credentials) {
    return std::any_of(credentials.begin(), credentials.end(),
        [](const RegisteredCredential& credential) {
            return credential.target > 0 && !isPs5Target(credential.target);
        });
}

struct PsConsole {
    std::string stable_id;
    std::string server_mac;
    std::string psn_duid;
    std::string nickname;
    int target = 0;

    std::optional<PsLocalEndpoint> local;
    std::optional<PsRemoteEndpoint> remote;
    std::optional<RegisteredCredential> credentials;

    bool registered() const { return credentials.has_value(); }
};

struct DiscoveredConsole {
    std::string host_addr;
    std::string host_name;
    std::string host_id;
    PsConsoleType console_type = PsConsoleType::Unknown;
    int target = 0;
    PsConsoleState state = PsConsoleState::Unknown;
    uint16_t host_request_port = 0;
    std::string system_version;
    std::string running_app_name;
};

} // namespace lunar::ps
