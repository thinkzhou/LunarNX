#include "ps/ps_remote_retry_policy.h"

#include <cassert>

using namespace lunar::ps;

int main() {
    assert(shouldRefreshPsnToken("session create", 401, false, true));
    assert(shouldRefreshPsnToken("session create", 403, false, true));
    assert(!shouldRefreshPsnToken("session create", 429, false, true));
    assert(!shouldRefreshPsnToken("session create", 500, false, true));
    assert(!shouldRefreshPsnToken("session start", 401, false, true));
    assert(!shouldRefreshPsnToken("session create", 0, false, true));
    assert(!shouldRefreshPsnToken("session create", 401, true, true));
    assert(!shouldRefreshPsnToken("session create", 401, false, false));
    assert(isRetryableRemoteHttpStatus(408));
    assert(isRetryableRemoteHttpStatus(429));
    assert(isRetryableRemoteHttpStatus(500));
    assert(!isRetryableRemoteHttpStatus(401));
    assert(!isRetryableRemoteHttpStatus(404));

    PsRemoteRetryPolicy policy;
    auto profile = policy.attemptProfile();
    assert(profile.mode == PsNatTraversalMode::Fast);
    assert(profile.port_guessing_sockets == 64);
    assert(profile.port_guesses == 0);

    // A random allocation observed on a successful CTRL offer upgrades the
    // pending DATA offer without requiring a failed media session first.
    PsRemoteRetryPolicy data_policy;
    data_policy.recordStunAllocation(false);
    assert(!data_policy.natCompatibilityEnabled());
    data_policy.recordStunAllocation(true);
    profile = data_policy.attemptProfile();
    assert(profile.mode == PsNatTraversalMode::Compatibility);

    // A transient PSN/WebSocket failure retries without adding NAT overhead.
    assert(policy.recordFailure("session create", true, true));
    profile = policy.attemptProfile();
    assert(profile.mode == PsNatTraversalMode::Fast);

    // Only a failed CTRL candidate check proves that NAT compatibility is
    // useful for the next attempt.
    assert(policy.recordFailure("control punch", true, true));
    profile = policy.attemptProfile();
    assert(profile.mode == PsNatTraversalMode::Compatibility);
    assert(profile.port_guessing_sockets == 120);
    assert(profile.port_guesses == 96);

    // Terminal and non-retryable failures do not schedule another attempt.
    PsRemoteRetryPolicy terminal;
    assert(!terminal.recordFailure("control punch", true, false));
    assert(!terminal.natCompatibilityEnabled());
    assert(!terminal.recordFailure("control punch", false, true));
    assert(!terminal.natCompatibilityEnabled());
    return 0;
}
