#pragma once

#include "../common.h"
#include "../api/http_client.h"
#include "token_store.h"
#include <string>
#include <functional>
#include <memory>
#include <chrono>

namespace lunar::auth {

struct StreamingToken {
    std::string gs_token;
    std::string base_uri;
    int duration_seconds = 0;
    bool valid() const { return !gs_token.empty(); }
};

enum class AuthState {
    Idle,
    WaitingForDeviceCode,   // User needs to visit microsoft.com/link
    Authenticating,         // Exchanging tokens
    Authenticated,          // Complete
    Error,
};

enum class DeviceCodePollResult {
    Pending,
    Authenticated,
    SlowDown,
    Declined,
    Expired,
    Error,
};

class AuthManager {
public:
    using StateCallback = std::function<void(AuthState state, const std::string& info)>;
    using CancelCallback = api::HttpClient::CancelCallback;

    AuthManager(api::HttpClient& http);
    ~AuthManager();

    // Start MSAL Device Code flow
    bool startDeviceCodeAuth();

    // Get the user code and verification URL
    std::string getUserCode() const { return user_code_; }
    std::string getVerificationUri() const { return verification_uri_; }
    std::string getAuthMessage() const { return auth_message_; }

    // Poll for token after user completes device code login
    bool pollForToken();
    DeviceCodePollResult pollForTokenResult();
    int getPollIntervalSeconds() const { return poll_interval_; }
    int getDeviceCodeExpiresInSeconds() const { return device_code_expires_in_; }

    // Token access
    bool isAuthenticated() const;
    bool hasSavedCredentials() const;
    bool refreshTokensIfNeeded(CancelCallback cancel = {});
    std::string getGssvToken() const;
    StreamingToken getHomeStreamingToken() const;
    StreamingToken getCloudStreamingToken() const;
    bool hasCloudAccess() const;
    // XStreaming-compatible region override for xCloud InvalidCountry (x-forwarded-for).
    void setForceRegionIp(const std::string& ip);
    std::string getForceRegionIp() const { return force_region_ip_; }
    // Re-run Xbox token derivation (home+cloud). Force bypasses 1-minute throttle.
    bool refreshStreamingTokens(bool force = false,
                                CancelCallback cancel = {});
    std::string getMsalAccessToken() const { return msal_access_token_; }
    // XStreaming "lpt" token for cloud ReadyToConnect /connect.
    // This is NOT the normal xboxlive.signin access token.
    std::string getXcloudTransferToken(bool force_refresh = false,
                                       CancelCallback cancel = {});
    std::string getWebToken() const { return web_token_; }
    std::string getUserHash() const { return user_hash_; }
    std::string getGamertag() const { return gamertag_; }
    std::string getLastError() const { return last_error_; }
    bool shouldRefreshTokens() const;

    // Persistence
    bool loadTokens(const std::string& path);
    bool saveTokens(const std::string& path);
    void clearTokens();

    // Callback for UI state updates
    void setStateCallback(StateCallback cb) { state_callback_ = std::move(cb); }

private:
    void setState(AuthState state, const std::string& info = "");
    void clearInMemoryTokens();
    bool hasUsableStreamingTokens() const;

    // Token chain steps
    bool stepGetStreamingTokens(CancelCallback cancel = {});
    bool fetchStreamToken(const std::string& xsts_token,
                          const std::string& offering_id,
                          const std::string& login_url,
                          StreamingToken* out_token,
                          bool required,
                          CancelCallback cancel = {});

    api::HttpClient& http_;
    TokenStore tokens_;
    StateCallback state_callback_;
    AuthState state_ = AuthState::Idle;

    // MSAL device code state
    std::string device_code_;
    std::string user_code_;
    std::string verification_uri_;
    std::string auth_message_;
    int poll_interval_ = 5;
    int device_code_expires_in_ = 900;

    // Access tokens
    std::string msal_access_token_;
    std::string msal_refresh_token_;

    // Derived tokens
    std::string web_token_;
    std::string gssv_token_;          // home gsToken (compat)
    StreamingToken home_token_;
    StreamingToken cloud_token_;
    std::string xsts_gssv_token_;
    std::string user_hash_;
    std::string gamertag_;
    std::string last_error_;
    std::string force_region_ip_;
    std::chrono::steady_clock::time_point last_refresh_attempt_{};
};

} // namespace lunar::auth
