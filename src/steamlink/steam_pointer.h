#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace lunar::steamlink {
enum class TouchMode { Off, Trackpad, Absolute };
enum class GyroMode { Off, Native, Mouse };
struct MotionSample {
    bool valid = false;
    std::array<float, 3> gyro{}, accel{}; // SDL axes, rad/s and m/s²
};
// libnx: rotations/s and g. Match the standard controller sensor frame.
inline MotionSample convertMotion(std::array<float, 3> angular, std::array<float, 3> gravity) {
    constexpr float tau = 6.28318530718f, g = 9.80665f;
    MotionSample s{true, {angular[0]*tau, angular[2]*tau, -angular[1]*tau},
                        {-gravity[0]*g, -gravity[2]*g, gravity[1]*g}};
    for (float v : s.gyro) if (!std::isfinite(v)) return {};
    for (float v : s.accel) if (!std::isfinite(v)) return {};
    return s;
}
struct TouchSample {
    bool valid = true;
    int count = 0;
    std::array<int, 2> id{}, x{}, y{};
};
struct PointerOutput {
    int dx = 0, dy = 0, wheel = 0;
    bool absolute = false, left = false, right = false;
    float x = 0, y = 0;
};
// Portable gesture state machine. Buttons are desired states, so the transport
// can retry failed edges without keeping a remote button stuck down.
class SteamPointer {
public:
    TouchMode touch_mode = TouchMode::Trackpad;
    GyroMode gyro_mode = GyroMode::Native;
    void fenceTouches() { blocked_=true; cancel(); }
    PointerOutput update(const TouchSample& t, const MotionSample& m,
                         bool game, bool aim, uint64_t ms) {
        PointerOutput out;
        const float dt = last_ms_ && ms >= last_ms_ ? std::min(ms-last_ms_, uint64_t(32))/1000.f : 0;
        last_ms_ = ms;
        if (!game) {
            blocked_ = blocked_ || (t.valid && t.count > 0) || count_ > 0;
            cancel(); return out;
        }
        if (t.valid) last_touch_ms_ = ms;
        if (!t.valid && ms-last_touch_ms_ > 100) { blocked_ = true; cancel(); }
        // Match StreamView's 96px right-edge menu gesture. A touch starting
        // here belongs entirely to the UI, even before the menu opens.
        if (t.valid && t.count>0 && !count_ && t.x[0]>=1184) {
            blocked_=true; cancel();
        }
        if (blocked_) {
            if (t.valid && t.count == 0) blocked_ = false;
        } else if (touch_mode != TouchMode::Off && t.valid) {
            const int count = std::clamp(t.count, 0, 2);
            if (!count) {
                if (count_ && !moved_ && !drag_ && ms-start_ms_ < 300) {
                    click_until_ = ms + 40;
                    right_click_ = multi_;
                }
                count_ = 0; drag_ = false; multi_ = false;
            } else {
                float x = t.x[0], y = t.y[0];
                if (count == 2) { x = (x+t.x[1])*0.5f; y = (y+t.y[1])*0.5f; }
                if (!count_ || count != count_ || t.id[0] != id_) {
                    const bool continuing = count_ != 0;
                    multi_ = continuing ? multi_ || count == 2 : count == 2;
                    // A two-to-one transition must not become a new left tap.
                    moved_ = continuing && moved_;
                    if (!continuing) start_ms_ = ms;
                    start_x_ = x; start_y_ = y;
                    prev_x_ = x; prev_y_ = y; id_ = t.id[0]; drag_ = false;
                }
                count_ = count;
                if (std::hypot(x-start_x_, y-start_y_) > 10) moved_ = true;
                if (count == 2) {
                    scroll_ += y-prev_y_;
                    out.wheel = std::clamp(int(scroll_/32), -4, 4);
                    scroll_ -= out.wheel*32;
                } else if (!multi_) {
                    if (touch_mode == TouchMode::Absolute) {
                        out.absolute = true;
                        out.x = std::clamp(x/1279.f, 0.f, 1.f);
                        out.y = std::clamp(y/719.f, 0.f, 1.f);
                    } else { rx_ += x-prev_x_; ry_ += y-prev_y_; }
                    if (!moved_ && ms-start_ms_ >= 400) drag_ = true;
                }
                prev_x_ = x; prev_y_ = y;
            }
        }
        if (gyro_mode == GyroMode::Mouse && aim && m.valid && !out.absolute) {
            const auto deadzone = [](float v) {
                return !std::isfinite(v) || std::abs(v) < 0.015f ? 0.f : std::clamp(v,-35.f,35.f);
            };
            rx_ -= deadzone(m.gyro[1])*dt*650.f;
            ry_ -= deadzone(m.gyro[0])*dt*650.f;
        }
        rx_=std::clamp(rx_,-512.f,512.f); ry_=std::clamp(ry_,-512.f,512.f);
        out.dx = int(rx_); out.dy = int(ry_);
        rx_ -= out.dx; ry_ -= out.dy;
        out.left = drag_ || (ms < click_until_ && !right_click_);
        out.right = ms < click_until_ && right_click_;
        return out;
    }
private:
    void cancel() { count_=0; drag_=false; multi_=false; click_until_=0; rx_=ry_=scroll_=0; }
    int count_=0, id_=0;
    bool blocked_=false, moved_=false, multi_=false, drag_=false, right_click_=false;
    float start_x_=0,start_y_=0,prev_x_=0,prev_y_=0,rx_=0,ry_=0,scroll_=0;
    uint64_t start_ms_=0,last_ms_=0,last_touch_ms_=0,click_until_=0;
};
} // namespace lunar::steamlink
