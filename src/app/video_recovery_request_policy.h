#pragma once

#include <cstdint>

namespace lunar::app {

// Bounded PLI cadence for one recovery episode. The first request is
// immediate, the next two cover a lost RTCP packet without paying the old
// one-second floor, and sustained server-side IDR delay falls back to 1 Hz.
class VideoRecoveryRequestPolicy {
public:
    void reset() {
        active_ = false;
        attempts_ = 0;
        last_attempt_ms_ = 0;
    }

    bool shouldRequest(bool recovery_active, uint64_t now_ms) {
        if (!recovery_active) {
            reset();
            return false;
        }
        if (!active_) {
            active_ = true;
            attempts_ = 0;
            last_attempt_ms_ = now_ms;
            return true;
        }
        if (attempts_ == 0) return true;
        const uint64_t delay_ms = attempts_ == 1 ? 300u :
            attempts_ == 2 ? 500u : 1000u;
        return now_ms >= last_attempt_ms_ &&
               now_ms - last_attempt_ms_ >= delay_ms;
    }

    void recordAttempt(uint64_t now_ms) {
        active_ = true;
        ++attempts_;
        last_attempt_ms_ = now_ms;
    }

    uint32_t attempts() const { return attempts_; }

private:
    bool active_ = false;
    uint32_t attempts_ = 0;
    uint64_t last_attempt_ms_ = 0;
};

} // namespace lunar::app
