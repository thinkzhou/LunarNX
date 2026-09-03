#pragma once

#include <string>

namespace lunar::app {

struct XboxIcePreference {
    std::string preferred_stun_url;
    std::string remote_address;
    int remote_port = 0;

    bool hasHomeRoute() const {
        return !remote_address.empty() && remote_port > 0 && remote_port <= 65535;
    }
};

class XboxIcePreferenceStore {
public:
    explicit XboxIcePreferenceStore(std::string path);
    XboxIcePreferenceStore();

    XboxIcePreference load(const std::string& server_id) const;
    bool save(const std::string& server_id,
              const XboxIcePreference& preference) const;

private:
    std::string path_;
};

} // namespace lunar::app
