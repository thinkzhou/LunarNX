#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace lunar::input {

enum class ButtonMappingProfile : uint8_t {
    Xbox,
    PlayStation,
};

enum class RemoteButton : uint8_t {
    A,
    B,
    X,
    Y,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Lb,
    Rb,
    Lt,
    Rt,
    L3,
    R3,
    View,
    Menu,
    Guide,
    Touchpad,
    Count,
};

constexpr size_t kRemoteButtonCount = static_cast<size_t>(RemoteButton::Count);
using ButtonMapping = std::array<uint64_t, kRemoteButtonCount>;

// Capture is a system button, not part of HidNpadButton. Bit 48 is outside the
// current libnx button range and still keeps combined masks exact in JSON.
constexpr uint64_t kButtonMappingCapture = uint64_t{1} << 48;

ButtonMapping defaultButtonMapping(ButtonMappingProfile profile);
ButtonMapping loadButtonMapping(ButtonMappingProfile profile);
bool saveButtonMapping(ButtonMappingProfile profile, const ButtonMapping& mapping);
const char* remoteButtonConfigKey(RemoteButton button);
std::string formatHidButtonMask(uint64_t mask);
bool mappingUsesCaptureButton(const ButtonMapping& mapping);
void acquireCaptureButtonInput();
void releaseCaptureButtonInput();
bool isCaptureButtonPressed();

} // namespace lunar::input
