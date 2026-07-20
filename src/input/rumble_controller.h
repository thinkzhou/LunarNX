#pragma once

#include "../common.h"
#include <array>
#include <cstdint>
#include <chrono>
#include <mutex>

namespace lunar::input {

// Handles Xbox 4-motor rumble protocol (2 grip + 2 trigger motors)
// Stores the last vibration state and fades out after duration.
// libnxbox reduces rumble by 50% (strong motors on Switch are intense).
class RumbleController {
public:
    RumbleController();
    ~RumbleController();

    // Initialize HD rumble hardware for the current stream session.
    bool initialize();

    // Called from WebRTC callback thread when Xbox sends vibration
    void setRumble(float left_motor, float right_motor,
                   float lt_motor, float rt_motor,
                   uint16_t duration_ms, uint16_t delay_ms, uint8_t repeat);

    // Called from main loop to apply vibration to hardware
    void update();

    // Stop all vibration
    void stop();

private:
    struct RumbleState {
        float left_motor    = 0.0f;  // 0.0-1.0
        float right_motor   = 0.0f;
        float lt_motor      = 0.0f;
        float rt_motor      = 0.0f;

        std::chrono::steady_clock::time_point start_time;
        uint16_t duration_ms  = 0;
        uint16_t delay_ms     = 0;
        uint8_t  repeat       = 0;

        bool active = false;
    };

    RumbleState state_;
    std::mutex mutex_;

#ifdef __SWITCH__
    bool hid_rumble_initialized_ = false;
    std::array<u64, 2> vibration_handles_{};
    int vibration_handle_count_ = 0;
    static constexpr float RUMBLE_SCALE = 0.5f;  // Reduce by 50% (libnxbox convention)
#endif
};

} // namespace lunar::input
