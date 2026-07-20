#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <mutex>

namespace lunar::auth {

struct SisuTokenData {
    std::string device_token;
    std::string title_token;
    std::string title_token_expires;
    std::string user_token;
    std::string user_token_expires;
    std::string authorization_token;
    std::string authorization_token_expires;
    std::string user_hash;    // uhs from UserToken DisplayClaims
    std::string gamertag;
};

struct TokenStoreData {
    // User token (from oauth20_token.srf)
    std::string access_token;
    std::string refresh_token;
    std::string user_id;
    std::string expires_on;
    int expires_in = 0;
    uint64_t expires_at_ms = 0;
    uint64_t token_issue_steady_ms = 0;

    // Sisu tokens
    SisuTokenData sisu;

    // Key material
    std::string jwk_x;     // EC P-256 public key X
    std::string jwk_y;     // EC P-256 public key Y
    std::string jwk_d;     // EC P-256 private key D

    // Streaming tokens
    std::string gssv_token;
    std::string gssv_base_uri;
    int gssv_duration_seconds = 0;
    std::string gssv_cloud_token;
    std::string gssv_cloud_base_uri;
    int gssv_cloud_duration_seconds = 0;
    std::string web_token;
    std::string gamertag;
    std::string user_hash;

    // Timestamps
    uint64_t token_update_time = 0;
};

class TokenStore {
public:
    TokenStore();
    ~TokenStore();

    // File I/O
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    void clear();

    // Token validity
    bool hasValidUserToken() const;
    bool hasValidSisuToken() const;

    // Accessors
    TokenStoreData& data() { return data_; }
    const TokenStoreData& data() const { return data_; }
    void setUserToken(const std::string& access_token, const std::string& refresh_token,
                      const std::string& user_id, int expires_in);
    void setSisuToken(const SisuTokenData& sisu);

private:
    TokenStoreData data_;
    mutable std::mutex mutex_;
};

} // namespace lunar::auth
