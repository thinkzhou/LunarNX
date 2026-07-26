#include "rumble_controller.h"
#include "../diagnostics.h"
#include <algorithm>
#include <atomic>
#include <cstdio>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace lunar::input {

namespace {
std::atomic<int> g_rumble_command_logs{0};
}

RumblePhase evaluateRumblePhase(uint64_t elapsed_ms,
                                uint16_t duration_ms,
                                uint16_t delay_ms,
                                uint8_t repeat) {
    const uint64_t effective_duration = duration_ms > 0 ? duration_ms : 30;
    const uint64_t cycle_duration = static_cast<uint64_t>(delay_ms) +
                                    effective_duration;
    const uint64_t cycle_count = static_cast<uint64_t>(repeat) + 1;
    if (elapsed_ms >= cycle_duration * cycle_count) {
        return RumblePhase::Finished;
    }
    return elapsed_ms % cycle_duration < delay_ms
        ? RumblePhase::Off
        : RumblePhase::On;
}

RumbleController::RumbleController() = default;

RumbleController::~RumbleController() {
    stop();
}

bool RumbleController::initialize() {
    stop();
    std::lock_guard<std::mutex> lock(mutex_);
    g_rumble_command_logs = 0;
    states_ = {};
#ifdef __SWITCH__
    handheld_device_ = {};
    player_devices_ = {};

    Result rc = hidInitializeVibrationDevices(
        reinterpret_cast<HidVibrationDeviceHandle*>(handheld_device_.handles.data()),
        2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    if (R_SUCCEEDED(rc)) {
        handheld_device_.handle_count = 2;
        handheld_device_.initialized = true;
        lunar::diagnosticLog("rumble", "HD rumble initialized handheld");
    }

    for (size_t i = 0; i < MAX_GAMEPADS; ++i) {
        auto& device = player_devices_[i];
        rc = hidInitializeVibrationDevices(
            reinterpret_cast<HidVibrationDeviceHandle*>(device.handles.data()),
            2, static_cast<HidNpadIdType>(i), HidNpadStyleTag_NpadJoyDual);
        if (R_SUCCEEDED(rc)) {
            device.handle_count = 2;
            device.initialized = true;
        }
    }
    const bool initialized = handheld_device_.initialized ||
                             player_devices_[0].initialized;
    lunar::diagnosticLog("rumble",
                         "HD rumble devices handheld=%s player1=%s",
                         handheld_device_.initialized ? "true" : "false",
                         player_devices_[0].initialized ? "true" : "false");
    return initialized;
#else
    return true;
#endif
}

void RumbleController::setRumble(uint8_t gamepad_index,
                                  float left_motor, float right_motor,
                                  float lt_motor, float rt_motor,
                                  uint16_t duration_ms, uint16_t delay_ms,
                                  uint8_t repeat) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (gamepad_index >= MAX_GAMEPADS) {
        lunar::diagnosticLog("rumble", "ignore gamepad index=%u", gamepad_index);
        return;
    }

    auto& state = states_[gamepad_index];
    state.left_motor = std::clamp(left_motor, 0.0f, 1.0f);
    state.right_motor = std::clamp(right_motor, 0.0f, 1.0f);
    state.lt_motor = std::clamp(lt_motor, 0.0f, 1.0f);
    state.rt_motor = std::clamp(rt_motor, 0.0f, 1.0f);
    state.duration_ms = duration_ms;
    state.delay_ms = delay_ms;
    state.repeat = repeat;
    state.start_time = std::chrono::steady_clock::now();
    state.active = enabled_ &&
        (state.left_motor > 0 || state.right_motor > 0 ||
         state.lt_motor > 0 || state.rt_motor > 0);
    state.dirty = true;

    if (g_rumble_command_logs.fetch_add(1) < 16) {
        lunar::diagnosticLog("rumble",
                             "command pad=%u left=%.0f right=%.0f lt=%.0f rt=%.0f duration=%u delay=%u repeat=%u",
                             gamepad_index,
                             left_motor * 100,
                             right_motor * 100,
                             lt_motor * 100,
                             rt_motor * 100,
                             duration_ms,
                             delay_ms,
                             repeat);
    }
}

void RumbleController::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled_) {
        for (auto& state : states_) {
            state.active = false;
            state.output_on = false;
            state.dirty = false;
        }
#ifdef __SWITCH__
        sendAllZeroLocked();
#endif
    }
}

void RumbleController::setStrengthPercent(int percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    strength_scale_ = static_cast<float>(std::clamp(percent, 0, 100)) / 100.0f;
    for (auto& state : states_) {
        if (state.active) state.dirty = true;
    }
}

void RumbleController::update() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < states_.size(); ++i) {
        auto& state = states_[i];
        bool desired_on = false;
        if (enabled_ && state.active) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - state.start_time).count();
            const auto phase = evaluateRumblePhase(
                elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0,
                state.duration_ms,
                state.delay_ms,
                state.repeat);
            desired_on = phase == RumblePhase::On;
            if (phase == RumblePhase::Finished) {
                state.active = false;
            }
        }

        if (desired_on != state.output_on || (desired_on && state.dirty)) {
#ifdef __SWITCH__
            sendStateLocked(i, desired_on);
#endif
            state.output_on = desired_on;
        }
        state.dirty = false;
    }
}

void RumbleController::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& state : states_) {
        state = {};
    }

#ifdef __SWITCH__
    sendAllZeroLocked();
#endif
}

#ifdef __SWITCH__
void RumbleController::sendStateLocked(size_t gamepad_index, bool enabled) {
    const auto& state = states_[gamepad_index];
    HidVibrationValue values[2] = {};
    if (enabled) {
        values[0].amp_low = state.left_motor * strength_scale_;
        values[0].freq_low = 160.0f;
        values[0].amp_high = state.lt_motor * strength_scale_;
        values[0].freq_high = 320.0f;
        values[1].amp_low = state.right_motor * strength_scale_;
        values[1].freq_low = 160.0f;
        values[1].amp_high = state.rt_motor * strength_scale_;
        values[1].freq_high = 320.0f;
    }

    auto send = [&values](VibrationDevice& device) {
        if (!device.initialized || device.handle_count <= 0) return;
        hidSendVibrationValues(
            reinterpret_cast<HidVibrationDeviceHandle*>(device.handles.data()),
            values,
            std::min(device.handle_count, 2));
    };
    if (gamepad_index == 0) send(handheld_device_);
    send(player_devices_[gamepad_index]);
}

void RumbleController::sendZeroLocked(VibrationDevice& device) {
    if (!device.initialized || device.handle_count <= 0) return;
    HidVibrationValue zero[2] = {};
    hidSendVibrationValues(
        reinterpret_cast<HidVibrationDeviceHandle*>(device.handles.data()),
        zero,
        std::min(device.handle_count, 2));
}

void RumbleController::sendAllZeroLocked() {
    sendZeroLocked(handheld_device_);
    for (auto& device : player_devices_) sendZeroLocked(device);
}
#endif

} // namespace lunar::input
