#include "rumble_controller.h"
#include "../diagnostics.h"
#include <atomic>
#include <cstdio>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace lunar::input {

namespace {
std::atomic<int> g_rumble_command_logs{0};
}

RumbleController::RumbleController() = default;

RumbleController::~RumbleController() {
    stop();
}

bool RumbleController::initialize() {
    stop();
    g_rumble_command_logs = 0;
#ifdef __SWITCH__
    hid_rumble_initialized_ = false;
    vibration_handle_count_ = 0;
    vibration_handles_.fill(0);

    Result rc = hidInitializeVibrationDevices(
        reinterpret_cast<HidVibrationDeviceHandle*>(vibration_handles_.data()),
        2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    if (R_SUCCEEDED(rc)) {
        vibration_handle_count_ = 2;
        hid_rumble_initialized_ = true;
        lunar::diagnosticLog("rumble", "HD rumble initialized handheld");
        return true;
    }
    // Try player 1 controller if handheld mode fails
    rc = hidInitializeVibrationDevices(
        reinterpret_cast<HidVibrationDeviceHandle*>(vibration_handles_.data()),
        2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    if (R_SUCCEEDED(rc)) {
        vibration_handle_count_ = 2;
        hid_rumble_initialized_ = true;
        lunar::diagnosticLog("rumble", "HD rumble initialized player1");
        return true;
    }
    lunar::diagnosticLog("rumble", "HD rumble init failed rc=0x%x", rc);
    return false;
#else
    return true;
#endif
}

void RumbleController::setRumble(float left_motor, float right_motor,
                                  float lt_motor, float rt_motor,
                                  uint16_t duration_ms, uint16_t delay_ms,
                                  uint8_t repeat) {
    std::lock_guard<std::mutex> lock(mutex_);

#ifdef __SWITCH__
    state_.left_motor  = left_motor  * RUMBLE_SCALE;
    state_.right_motor = right_motor * RUMBLE_SCALE;
    state_.lt_motor    = lt_motor    * RUMBLE_SCALE;
    state_.rt_motor    = rt_motor    * RUMBLE_SCALE;
#else
    state_.left_motor  = left_motor;
    state_.right_motor = right_motor;
    state_.lt_motor    = lt_motor;
    state_.rt_motor    = rt_motor;
#endif

    state_.duration_ms = duration_ms;
    state_.delay_ms    = delay_ms;
    state_.repeat      = repeat;
    state_.start_time  = std::chrono::steady_clock::now();
    state_.active      = true;

    if (g_rumble_command_logs.fetch_add(1) < 16) {
        lunar::diagnosticLog("rumble",
                             "command left=%.0f right=%.0f lt=%.0f rt=%.0f duration=%u delay=%u repeat=%u",
                             left_motor * 100,
                             right_motor * 100,
                             lt_motor * 100,
                             rt_motor * 100,
                             duration_ms,
                             delay_ms,
                             repeat);
    }
}

void RumbleController::update() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!state_.active) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state_.start_time).count();
#ifdef __SWITCH__
    bool needs_zero_packet = false;
#endif

    // Suppress output during delay period (start after delay_ms)
    if (elapsed < state_.delay_ms) return;

    // Check if rumble duration has expired
    auto total_duration = state_.delay_ms + state_.duration_ms;
    if (elapsed > total_duration) {
        if (state_.repeat > 0) {
            state_.repeat--;
            state_.start_time = now;
        } else {
            state_.active = false;
            state_.left_motor = 0;
            state_.right_motor = 0;
            state_.lt_motor = 0;
            state_.rt_motor = 0;
#ifdef __SWITCH__
            needs_zero_packet = true;
#endif
        }
    }

#ifdef __SWITCH__
    if (hid_rumble_initialized_ && vibration_handle_count_ > 0) {
        // Skip HID call if all motors are idle (common case, no vibration).
        // Natural expiry still needs one zero packet to stop already-running motors.
        if (state_.left_motor == 0 && state_.right_motor == 0 &&
            state_.lt_motor == 0 && state_.rt_motor == 0 && !needs_zero_packet) return;

        auto* handles = reinterpret_cast<HidVibrationDeviceHandle*>(vibration_handles_.data());

        HidVibrationValue values[2] = {};
        values[0].amp_low  = state_.left_motor;
        values[0].freq_low = 160.0f;
        values[0].amp_high = state_.lt_motor;
        values[0].freq_high = 320.0f;
        values[1].amp_low  = state_.right_motor;
        values[1].freq_low = 160.0f;
        values[1].amp_high = state_.rt_motor;
        values[1].freq_high = 320.0f;

        hidSendVibrationValues(handles, values,
            vibration_handle_count_ > 2 ? 2 : vibration_handle_count_);
    }
#endif
}

void RumbleController::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.active = false;
    state_.left_motor = 0;
    state_.right_motor = 0;
    state_.lt_motor = 0;
    state_.rt_motor = 0;

#ifdef __SWITCH__
    if (hid_rumble_initialized_ && vibration_handle_count_ > 0) {
        auto* handles = reinterpret_cast<HidVibrationDeviceHandle*>(vibration_handles_.data());
        HidVibrationValue zero[2] = {};
        hidSendVibrationValues(handles, zero, vibration_handle_count_);
    }
#endif
}

} // namespace lunar::input
