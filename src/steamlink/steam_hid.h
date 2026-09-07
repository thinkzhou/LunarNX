#pragma once
#include "../input/gamepad_reader.h"
extern "C" {
#include <ihslib/hid.h>
}
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace lunar::steamlink {
using SteamPadReport = std::array<uint8_t, 48>;
// Wire layout follows vendored ihslib's sdl_hid_report.h (generic gamepad).
SteamPadReport encodeSteamPad(const input::GamepadState& state);
struct SteamPadState {
    std::mutex mutex;
    SteamPadReport report{};
    uint16_t rumble_low = 0, rumble_high = 0;
    uint32_t rumble_duration = 0;
    uint64_t rumble_generation = 0;
    std::atomic<uint64_t> reports{0};
    std::atomic<bool> opened{false};
    void publish(const input::GamepadState& state);
};
// Provider remains caller-owned. Destroy it only after IHS_SessionDestroy.
IHS_HIDProvider* createSteamPadProvider(std::shared_ptr<SteamPadState> state);
void destroySteamPadProvider(IHS_HIDProvider* provider);
} // namespace lunar::steamlink
