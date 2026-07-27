#pragma once

#include <chrono>

namespace lunar::app {

class VideoRecoveryTransportRetry {
public:
    static constexpr std::chrono::milliseconds kInterval{200};

    bool shouldRetry(bool waiting_for_keyframe,
                     std::chrono::steady_clock::time_point now) {
        if (!waiting_for_keyframe) {
            active_ = false;
            return false;
        }
        if (!active_ || now < last_attempt_ || now - last_attempt_ >= kInterval) {
            active_ = true;
            last_attempt_ = now;
            return true;
        }
        return false;
    }

private:
    bool active_ = false;
    std::chrono::steady_clock::time_point last_attempt_{};
};

} // namespace lunar::app
