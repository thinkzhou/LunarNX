#include "button_mapping.h"
#include "../common.h"
#include <cJSON.h>
#include <borealis/core/application.hpp>
#include <borealis/core/platform.hpp>
#include <borealis/platforms/switch/switch_input.hpp>
#include <switch.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <vector>

namespace lunar::input {
namespace {

struct ButtonName {
    uint64_t mask;
    const char* name;
};

constexpr ButtonName kButtons[] = {
    {HidNpadButton_A, "A"}, {HidNpadButton_B, "B"},
    {HidNpadButton_X, "X"}, {HidNpadButton_Y, "Y"},
    {HidNpadButton_L, "L"}, {HidNpadButton_R, "R"},
    {HidNpadButton_ZL, "ZL"}, {HidNpadButton_ZR, "ZR"},
    {HidNpadButton_StickL, "L3"}, {HidNpadButton_StickR, "R3"},
    {HidNpadButton_Minus, "Minus"}, {HidNpadButton_Plus, "Plus"},
    {HidNpadButton_Up, "D-Up"}, {HidNpadButton_Down, "D-Down"},
    {HidNpadButton_Left, "D-Left"}, {HidNpadButton_Right, "D-Right"},
    {kButtonMappingCapture, "Capture"},
};

std::mutex g_capture_button_mutex;
size_t g_capture_button_users = 0;
brls::ButtonOverrideMode g_previous_capture_button_mode =
    brls::ButtonOverrideMode::NONE;

brls::SwitchInputManager* switchInputManager() {
    auto* platform = brls::Application::getPlatform();
    if (!platform) return nullptr;
    return static_cast<brls::SwitchInputManager*>(platform->getInputManager());
}

cJSON* readConfig() {
    FILE* file = std::fopen(lunar::get_config_path(), "rb");
    if (!file) return cJSON_CreateObject();
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    std::string data(size > 0 ? static_cast<size_t>(size) : 0, '\0');
    if (!data.empty()) std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    cJSON* root = cJSON_Parse(data.c_str());
    return root ? root : cJSON_CreateObject();
}

bool writeConfig(cJSON* root) {
    char* text = cJSON_Print(root);
    if (!text) return false;
    FILE* file = std::fopen(lunar::get_config_path(), "wb");
    const bool ok = file && std::fwrite(text, 1, std::strlen(text), file) == std::strlen(text);
    if (file) std::fclose(file);
    cJSON_free(text);
    return ok;
}

} // namespace

const char* mappingConfigKey(ButtonMappingProfile profile) {
    return profile == ButtonMappingProfile::PlayStation
        ? "ps_button_mapping"
        : "xbox_button_mapping";
}

ButtonMapping defaultButtonMapping(ButtonMappingProfile profile) {
    ButtonMapping mapping{};
    mapping[static_cast<size_t>(RemoteButton::A)] = HidNpadButton_B;
    mapping[static_cast<size_t>(RemoteButton::B)] = HidNpadButton_A;
    mapping[static_cast<size_t>(RemoteButton::X)] = HidNpadButton_Y;
    mapping[static_cast<size_t>(RemoteButton::Y)] = HidNpadButton_X;
    mapping[static_cast<size_t>(RemoteButton::DpadUp)] = HidNpadButton_Up;
    mapping[static_cast<size_t>(RemoteButton::DpadDown)] = HidNpadButton_Down;
    mapping[static_cast<size_t>(RemoteButton::DpadLeft)] = HidNpadButton_Left;
    mapping[static_cast<size_t>(RemoteButton::DpadRight)] = HidNpadButton_Right;
    mapping[static_cast<size_t>(RemoteButton::Lb)] = HidNpadButton_L;
    mapping[static_cast<size_t>(RemoteButton::Rb)] = HidNpadButton_R;
    mapping[static_cast<size_t>(RemoteButton::Lt)] = HidNpadButton_ZL;
    mapping[static_cast<size_t>(RemoteButton::Rt)] = HidNpadButton_ZR;
    mapping[static_cast<size_t>(RemoteButton::L3)] = HidNpadButton_StickL;
    mapping[static_cast<size_t>(RemoteButton::R3)] = HidNpadButton_StickR;
    mapping[static_cast<size_t>(RemoteButton::View)] = HidNpadButton_Minus;
    mapping[static_cast<size_t>(RemoteButton::Menu)] = HidNpadButton_Plus;
    mapping[static_cast<size_t>(RemoteButton::Guide)] =
        HidNpadButton_L | HidNpadButton_R | HidNpadButton_Plus;
    if (profile == ButtonMappingProfile::PlayStation) {
        mapping[static_cast<size_t>(RemoteButton::Touchpad)] = 0;
    }
    return mapping;
}

const char* remoteButtonConfigKey(RemoteButton button) {
    constexpr const char* keys[] = {
        "a", "b", "x", "y", "dpad_up", "dpad_down", "dpad_left",
        "dpad_right", "lb", "rb", "lt", "rt", "l3", "r3", "view",
        "menu", "guide", "touchpad",
    };
    return keys[static_cast<size_t>(button)];
}

ButtonMapping loadButtonMapping(ButtonMappingProfile profile) {
    ButtonMapping mapping = defaultButtonMapping(profile);
    cJSON* root = readConfig();
    const cJSON* object = cJSON_GetObjectItemCaseSensitive(
        root, mappingConfigKey(profile));
    // The former shared setting represented the Xbox-shaped input state. Keep
    // it as an Xbox-only migration fallback; PlayStation starts independently.
    if (!cJSON_IsObject(object) && profile == ButtonMappingProfile::Xbox) {
        object = cJSON_GetObjectItemCaseSensitive(root, "button_mapping");
    }
    if (cJSON_IsObject(object)) {
        for (size_t i = 0; i < mapping.size(); ++i) {
            const cJSON* value = cJSON_GetObjectItemCaseSensitive(
                object, remoteButtonConfigKey(static_cast<RemoteButton>(i)));
            if (cJSON_IsNumber(value) && value->valuedouble >= 0) {
                mapping[i] = static_cast<uint64_t>(value->valuedouble);
            }
        }
    }
    cJSON_Delete(root);
    return mapping;
}

bool saveButtonMapping(ButtonMappingProfile profile, const ButtonMapping& mapping) {
    cJSON* root = readConfig();
    const char* config_key = mappingConfigKey(profile);
    cJSON_DeleteItemFromObject(root, config_key);
    cJSON* object = cJSON_CreateObject();
    for (size_t i = 0; i < mapping.size(); ++i) {
        cJSON_AddNumberToObject(object,
            remoteButtonConfigKey(static_cast<RemoteButton>(i)),
            static_cast<double>(mapping[i]));
    }
    cJSON_AddItemToObject(root, config_key, object);
    const bool ok = writeConfig(root);
    cJSON_Delete(root);
    return ok;
}

std::string formatHidButtonMask(uint64_t mask) {
    if (mask == 0) return "Not mapped";
    std::string result;
    for (const auto& button : kButtons) {
        if ((mask & button.mask) == 0) continue;
        if (!result.empty()) result += " + ";
        result += button.name;
    }
    return result.empty() ? "?" : result;
}

bool mappingUsesCaptureButton(const ButtonMapping& mapping) {
    return std::any_of(mapping.begin(), mapping.end(), [](uint64_t mask) {
        return (mask & kButtonMappingCapture) != 0;
    });
}

void acquireCaptureButtonInput() {
    std::lock_guard<std::mutex> lock(g_capture_button_mutex);
    auto* input = switchInputManager();
    if (!input) return;
    if (g_capture_button_users++ == 0) {
        g_previous_capture_button_mode = input->screenshotButtonOverrideMode();
        appletSetRequiresCaptureButtonShortPressedMessage(true);
        input->setScreenshotButtonOverrideMode(brls::ButtonOverrideMode::CUSTOM_EVENT);
    }
}

void releaseCaptureButtonInput() {
    std::lock_guard<std::mutex> lock(g_capture_button_mutex);
    if (g_capture_button_users == 0) return;
    if (--g_capture_button_users != 0) return;
    auto* input = switchInputManager();
    if (!input) return;
    input->setScreenshotButtonOverrideMode(g_previous_capture_button_mode);
    appletSetRequiresCaptureButtonShortPressedMessage(false);
}

bool isCaptureButtonPressed() {
    auto* input = switchInputManager();
    return input && input->isScreenshotButtonPressed();
}

} // namespace lunar::input
