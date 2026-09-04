#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lunar::ps {

struct PsRoutePreference {
    std::string preferred_stun_host;
    int preferred_stun_port = 0;
    std::string remote_address;
    int remote_port = 0;

    bool hasPreferredStun() const {
        return !preferred_stun_host.empty() && preferred_stun_port > 0 &&
            preferred_stun_port <= 65535;
    }

    bool hasRemoteRoute() const {
        return !remote_address.empty() && remote_port > 0 && remote_port <= 65535;
    }
};

std::string psRoutePreferenceKey(const uint8_t* console_uid, size_t size);

class PsRoutePreferenceStore {
public:
    explicit PsRoutePreferenceStore(std::string path);
    PsRoutePreferenceStore();

    PsRoutePreference load(const std::string& console_key) const;
    bool save(const std::string& console_key,
              const PsRoutePreference& preference) const;

private:
    std::string path_;
};

} // namespace lunar::ps
