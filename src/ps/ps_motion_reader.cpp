#ifdef __SWITCH__

#include "ps_motion_reader.h"
#include "../diagnostics.h"
#include <switch.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <new>

namespace lunar::ps {
namespace {

constexpr float kTau = 6.28318530717958647692f;

PsMotionState convertSixAxis(const HidSixAxisSensorState& sixaxis) {
    PsMotionState state{};
    state.valid = true;
    // libnx angular velocity is in revolutions/s; Chiaki expects rad/s.
    // Match chiaki-ng's Switch-to-DualShock coordinate transform.
    state.gyro_x = sixaxis.angular_velocity.x * kTau;
    state.gyro_y = sixaxis.angular_velocity.z * kTau;
    state.gyro_z = -sixaxis.angular_velocity.y * kTau;
    state.accel_x = -sixaxis.acceleration.x;
    state.accel_y = -sixaxis.acceleration.z;
    state.accel_z = sixaxis.acceleration.y;

    const float (*dm)[3] = sixaxis.direction.direction;
    const float m[3][3] = {
        {dm[0][0], dm[2][0], dm[1][0]},
        {dm[0][2], dm[2][2], dm[1][2]},
        {dm[0][1], dm[2][1], dm[1][1]},
    };
    std::array<float, 4> q{};
    float t = 0.0f;
    if (m[2][2] < 0.0f) {
        if (m[0][0] > m[1][1]) {
            t = 1.0f + m[0][0] - m[1][1] - m[2][2];
            q = {t, m[0][1] + m[1][0], m[2][0] + m[0][2],
                 m[1][2] - m[2][1]};
        } else {
            t = 1.0f - m[0][0] + m[1][1] - m[2][2];
            q = {m[0][1] + m[1][0], t, m[1][2] + m[2][1],
                 m[2][0] - m[0][2]};
        }
    } else if (m[0][0] < -m[1][1]) {
        t = 1.0f - m[0][0] - m[1][1] + m[2][2];
        q = {m[2][0] + m[0][2], m[1][2] + m[2][1], t,
             m[0][1] - m[1][0]};
    } else {
        t = 1.0f + m[0][0] + m[1][1] + m[2][2];
        q = {m[1][2] - m[2][1], m[2][0] - m[0][2],
             m[0][1] - m[1][0], t};
    }
    if (t > 0.000001f) {
        const float factor = 0.5f / std::sqrt(t);
        state.orient_x = q[0] * factor;
        state.orient_y = q[1] * factor;
        state.orient_z = -q[2] * factor;
        state.orient_w = q[3] * factor;
    }
    return state;
}

} // namespace

struct PsMotionReader::Impl {
    PadState pad{};
    std::array<HidSixAxisSensorHandle, 4> handles{};
    std::array<bool, 4> started{};
    PsMotionState last_state{};
    bool initialized = false;
};

PsMotionReader::PsMotionReader() : impl_(new (std::nothrow) Impl()) {}
PsMotionReader::~PsMotionReader() { reset(); delete impl_; }

bool PsMotionReader::initialize() {
    if (!impl_) return false;
    if (impl_->initialized) return true;
    padInitializeDefault(&impl_->pad);
    const Result results[3] = {
        hidGetSixAxisSensorHandles(&impl_->handles[0], 1, HidNpadIdType_Handheld,
                                   HidNpadStyleTag_NpadHandheld),
        hidGetSixAxisSensorHandles(&impl_->handles[1], 1, HidNpadIdType_No1,
                                   HidNpadStyleTag_NpadFullKey),
        hidGetSixAxisSensorHandles(&impl_->handles[2], 2, HidNpadIdType_No1,
                                   HidNpadStyleTag_NpadJoyDual),
    };
    const bool valid[4] = {R_SUCCEEDED(results[0]), R_SUCCEEDED(results[1]),
                           R_SUCCEEDED(results[2]), R_SUCCEEDED(results[2])};
    for (size_t i = 0; i < impl_->handles.size(); ++i) {
        if (valid[i] && R_SUCCEEDED(hidStartSixAxisSensor(impl_->handles[i])))
            impl_->started[i] = true;
    }
    impl_->initialized = std::any_of(impl_->started.begin(), impl_->started.end(),
                                    [](bool value) { return value; });
    lunar::diagnosticLog("ps-motion", "six-axis initialize started=%d%d%d%d",
                         impl_->started[0], impl_->started[1],
                         impl_->started[2], impl_->started[3]);
    return impl_->initialized;
}

PsMotionState PsMotionReader::read(bool suppressed) {
    if (!impl_ || !impl_->initialized || suppressed) return {};
    padUpdate(&impl_->pad);
    size_t index = impl_->handles.size();
    const uint64_t style = padGetStyleSet(&impl_->pad);
    if ((style & HidNpadStyleTag_NpadHandheld) && impl_->started[0]) index = 0;
    else if ((style & HidNpadStyleTag_NpadFullKey) && impl_->started[1]) index = 1;
    else if (style & HidNpadStyleTag_NpadJoyDual) {
        const uint64_t attributes = padGetAttributes(&impl_->pad);
        if ((attributes & HidNpadAttribute_IsLeftConnected) && impl_->started[2])
            index = 2;
        else if ((attributes & HidNpadAttribute_IsRightConnected) && impl_->started[3])
            index = 3;
    }
    if (index >= impl_->handles.size()) return impl_->last_state;
    HidSixAxisSensorState sixaxis{};
    if (hidGetSixAxisSensorStates(impl_->handles[index], &sixaxis, 1) == 0)
        return impl_->last_state;
    impl_->last_state = convertSixAxis(sixaxis);
    return impl_->last_state;
}

void PsMotionReader::reset() {
    if (!impl_) return;
    for (size_t i = 0; i < impl_->handles.size(); ++i)
        if (impl_->started[i]) hidStopSixAxisSensor(impl_->handles[i]);
    impl_->started.fill(false);
    impl_->last_state = {};
    impl_->initialized = false;
}

} // namespace lunar::ps

#endif
