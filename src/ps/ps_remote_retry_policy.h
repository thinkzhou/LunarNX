#pragma once

#include <string_view>

namespace lunar::ps {

constexpr int kPsFastNatSockets = 64;
constexpr int kPsCompatibilityNatSockets = 120;
constexpr int kPsCompatibilityPortGuesses = 96;

enum class PsNatTraversalMode {
    Fast,
    Compatibility,
};

struct PsRemoteAttemptProfile {
    PsNatTraversalMode mode = PsNatTraversalMode::Fast;
    int port_guessing_sockets = kPsFastNatSockets;
    int port_guesses = 0;
};

inline bool shouldRefreshPsnToken(std::string_view phase,
                                  long http_status,
                                  bool refresh_already_attempted,
                                  bool has_attempt_remaining) {
    // A rejected session-creation request is the only HTTP failure we can
    // safely interpret as a possibly stale access token. Later PS4 startup
    // failures may be a deterministic "no Main PS4" response instead.
    return (http_status == 401 || http_status == 403) &&
        phase == "session create" &&
        !refresh_already_attempted && has_attempt_remaining;
}

inline bool isRetryableRemoteHttpStatus(long http_status) {
    return http_status == 408 || http_status == 429 || http_status >= 500;
}

// Escalation is deliberately stateful and monotonic. Ordinary PSN/HTTP
// retries remain on the low-overhead path; only a retryable failure after the
// CTRL candidates were tested enables wider bounded port guessing.
class PsRemoteRetryPolicy {
public:
    PsRemoteRetryPolicy(
        int fast_sockets = kPsFastNatSockets,
        int compatibility_sockets = kPsCompatibilityNatSockets)
        : fast_sockets_(fast_sockets)
        , compatibility_sockets_(compatibility_sockets) {}

    PsRemoteAttemptProfile attemptProfile() const {
        if (!nat_compatibility_) {
            return {PsNatTraversalMode::Fast, fast_sockets_, 0};
        }
        return {
            PsNatTraversalMode::Compatibility,
            compatibility_sockets_,
            kPsCompatibilityPortGuesses,
        };
    }

    bool recordFailure(std::string_view phase, bool retryable,
                       bool has_attempt_remaining) {
        if (!retryable || !has_attempt_remaining) return false;
        if (phase == "control punch") nat_compatibility_ = true;
        return true;
    }

    // A successful CTRL punch can still reveal random external port
    // allocation. Promote before Chiaki creates its DATA offer so that media
    // does not fail after an apparently successful control bootstrap.
    void recordStunAllocation(bool random_allocation) {
        if (random_allocation) nat_compatibility_ = true;
    }

    bool natCompatibilityEnabled() const { return nat_compatibility_; }

private:
    int fast_sockets_;
    int compatibility_sockets_;
    bool nat_compatibility_ = false;
};

} // namespace lunar::ps
