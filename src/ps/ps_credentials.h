#pragma once

#if defined(__SWITCH__) || defined(LUNARNX_DESKTOP_TEST)

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
    // Replace one credential only after the complete credential set has been
    // durably written. A failed save leaves the in-memory set unchanged.
    bool addAndSave(const RegisteredCredential& cred, const std::string& path);
    bool updateLastKnownAddrAndSave(const std::string& server_mac,
                                    const std::string& address,
                                    const std::string& path);
    void add(const RegisteredCredential& cred);
    void remove(const std::string& server_mac);
    bool empty() const;

private:
    std::vector<RegisteredCredential> hosts_;
    mutable std::mutex mutex_;
};

} // namespace lunar::ps

#endif
