#pragma once

#ifdef __SWITCH__

#include <chiaki/remote/holepunch.h>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace lunar::ps {

enum class PsnAuthState { Idle, WaitingForCode, ExchangingCode, Authenticated, Error };

class PsnAuthManager {
public:
    using StateCallback = std::function<void(PsnAuthState, const std::string&)>;

    PsnAuthManager();
    ~PsnAuthManager();

    // Generate DUID + Sony OAuth URL. Call once, reuses existing DUID.
    std::string startAuth();

    // Opens the Switch Web Applet and captures Sony's callback code. The applet
    // call is blocking; token exchange should be dispatched to a network worker.
    bool openWebApplet(std::string& authorization_code);

    // Mode B: User pastes/enters the redirect URL manually.
    // Extracts code from URL and exchanges for token.
    bool submitRedirectUrl(const std::string& url, StateCallback cb);

    // Exchange authorization code for PSN token (shared by both modes).
    bool exchangeCodeForToken(const std::string& code, StateCallback cb);

    bool isAuthenticated() const { return state_.load() == PsnAuthState::Authenticated; }
    bool hasValidToken() const;
    bool hasStoredSession() const;
    bool ensureValidToken(StateCallback cb = {});
    bool refreshToken(StateCallback cb = {});
    bool refreshIdentity();  // re-fetch accountId + onlineId with the current token
    bool loadToken(const std::string& path);
    bool saveToken(const std::string& path) const;
    void signOut();

    std::string getAccessToken() const;
    std::string getAccountId() const;
    std::string getOnlineId() const;
    std::string getDuid() const;
    std::string getAuthError() const;
    PsnAuthState getState() const { return state_.load(); }
    std::string getLoginUrl() const;

private:
    bool fetchAccountId();
    bool refreshAccessToken();
    bool requestToken(const std::map<std::string, std::string>& params,
                      bool preserve_refresh_token);
    bool fail(const std::string& message, StateCallback cb = {});

    mutable std::mutex mutex_;
    std::mutex request_mutex_;
    std::string access_token_;
    std::string refresh_token_;
    std::string account_id_;
    std::string online_id_;
    std::string duid_;
    int expires_in_ = 3600;
    uint64_t expires_at_ms_ = 0;

    std::atomic<PsnAuthState> state_{PsnAuthState::Idle};
    std::string error_;
    std::string login_url_;
};

} // namespace lunar::ps

#endif
