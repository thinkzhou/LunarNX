#pragma once
#include "steam_pointer.h"
#ifdef __SWITCH__
#include <switch.h>
namespace lunar::steamlink {
class SteamSensors {
public:
    explicit SteamSensors(bool motion_enabled);
    ~SteamSensors();
    MotionSample read();
    TouchSample touch();
private:
    PadState pad_{};
    std::array<HidSixAxisSensorHandle,4> handles_{};
    std::array<bool,4> started_{};
    std::array<float,3> bias_{}, sum_{};
    unsigned calibration_=0;
    int last_source_=-1;
    uint64_t last_sample_=0;
    uint64_t last_fresh_ms_=0;
};
}
#endif
