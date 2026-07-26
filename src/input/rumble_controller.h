#pragma once

#include "../common.h"
#include <array>
#include <cstdint>
#include <chrono>
#include <mutex>

namespace lunar::input {

enum class RumblePhase {
    Off,
    On,
    Finished,
};

RumblePhase evaluateRumblePhase(uint64_t elapsed_ms,
                                uint16_t duration_ms,
                                uint16_t delay_ms,
                                uint8_t repeat);

// Handles Xbox 4-motor rumble protocol (2 grip + 2 trigger motors)
// Stores independent commands for each Xbox gamepad index and maps them to
// Switch HD rumble devices.
class RumbleController {
public:
    RumbleController();
    ~RumbleController();

    // Initialize HD rumble hardware for the current stream session.
    bool initialize();

    // Called from WebRTC callback thread when Xbox sends vibration
    void setRumble(uint8_t gamepad_index,
                   float left_motor, float right_motor,
                   float lt_motor, float rt_motor,
                   uint16_t duration_ms, uint16_t delay_ms, uint8_t repeat);

    void setEnabled(bool enabled);
    void setStrengthPercent(int percent);

    // Called from main loop to apply vibration to hardware
    void update();

    // Stop all vibration
    void stop();

private:
    static constexpr size_t MAX_GAMEPADS = 8;

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
        bool output_on = false;
        bool dirty = false;
    };

    std::array<RumbleState, MAX_GAMEPADS> states_{};
    std::mutex mutex_;
    bool enabled_ = true;
    float strength_scale_ = 0.5f;

#ifdef __SWITCH__
    struct VibrationDevice {
        std::array<u64, 2> handles{};
        int handle_count = 0;
        bool initialized = false;
    };

    VibrationDevice handheld_device_{};
    std::array<VibrationDevice, MAX_GAMEPADS> player_devices_{};

    void sendStateLocked(size_t gamepad_index, bool enabled);
    void sendZeroLocked(VibrationDevice& device);
    void sendAllZeroLocked();
#endif
};

} // namespace lunar::input
