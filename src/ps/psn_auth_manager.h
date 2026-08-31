#pragma once

#ifdef __SWITCH__

#include "../common/operation_generation.h"
#include <chiaki/remote/holepunch.h>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace lunar::ps {

enum class PsnAuthState { Idle, WaitingForCode, ExchangingCode, Authenticated, Error };
enum class PsnAuthErrorKind { None, Cancelled, Transient, SessionExpired, Fatal };

class PsnAuthManager {
public:
    using StateCallback = std::function<void(PsnAuthState, const std::string&)>;
    using CancelCallback = std::function<bool()>;

    PsnAuthManager();
    ~PsnAuthManager();

    // Generate DUID + Sony OAuth URL. Call once, reuses existing DUID.
    std::string startAuth();

    // Opens the Switch Web Applet and captures Sony's callback code. The applet
    // call is blocking; token exchange should be dispatched to a network worker.
    bool openWebApplet(std::string& authorization_code);

    // Mode B: User pastes/enters the redirect URL manually.
    // Extracts code from URL and exchanges for token.
    bool submitRedirectUrl(const std::string& url, StateCallback cb,
                           CancelCallback cancel = {});

    // Exchange authorization code for PSN token (shared by both modes).
    bool exchangeCodeForToken(const std::string& code, StateCallback cb,
                              CancelCallback cancel = {});

    bool isAuthenticated() const { return state_.load() == PsnAuthState::Authenticated; }
    bool hasValidToken() const;
    bool hasStoredSession() const;
    bool ensureValidToken(StateCallback cb = {}, bool* refreshed = nullptr,
                          CancelCallback cancel = {});
    bool refreshToken(StateCallback cb = {}, CancelCallback cancel = {});
    bool refreshIdentity(CancelCallback cancel = {});
    bool loadToken(const std::string& path);
    bool saveToken(const std::string& path) const;
    void signOut();

    std::string getAccessToken() const;
    std::string getAccountId() const;
    std::string getOnlineId() const;
    std::string getDuid() const;
    std::string getAuthError() const;
    PsnAuthErrorKind getAuthErrorKind() const;
    PsnAuthState getState() const { return state_.load(); }
    std::string getLoginUrl() const;

private:
    using RequestTicket = common::OperationGeneration::Ticket;
    bool fetchAccountId(CancelCallback cancel, RequestTicket ticket);
    bool refreshAccessToken(CancelCallback cancel, RequestTicket ticket);
    bool requestToken(const std::map<std::string, std::string>& params,
                      bool preserve_refresh_token,
                      CancelCallback cancel, RequestTicket ticket);
    bool requestCancelled(const CancelCallback& cancel,
                          RequestTicket ticket) const;
    bool failRequest(const std::string& message, PsnAuthErrorKind kind,
                     RequestTicket ticket, StateCallback cb = {});
    bool markAuthenticated(RequestTicket ticket);
    bool fail(const std::string& message, StateCallback cb = {},
              PsnAuthErrorKind kind = PsnAuthErrorKind::Fatal);

    mutable std::mutex mutex_;
    std::mutex request_mutex_;
    common::OperationGeneration request_generation_;
    std::string access_token_;
    std::string refresh_token_;
    std::string account_id_;
    std::string online_id_;
    std::string duid_;
    int expires_in_ = 3600;
    uint64_t expires_at_ms_ = 0;

    std::atomic<PsnAuthState> state_{PsnAuthState::Idle};
    std::string error_;
    PsnAuthErrorKind error_kind_ = PsnAuthErrorKind::None;
    std::string login_url_;
};

} // namespace lunar::ps

#endif
