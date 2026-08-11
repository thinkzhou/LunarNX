#pragma once

#ifdef __SWITCH__

#include "ps_console.h"
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lunar::ps {

class PsCredentials {
public:
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    void clear();

    std::vector<RegisteredCredential> getRegisteredHosts() const;
    std::optional<RegisteredCredential> findByMac(const std::string& server_mac) const;
    void add(const RegisteredCredential& cred);
    void remove(const std::string& server_mac);
    bool empty() const;

private:
    std::vector<RegisteredCredential> hosts_;
    mutable std::mutex mutex_;
};

} // namespace lunar::ps

#endif
