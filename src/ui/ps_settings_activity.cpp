#ifdef __SWITCH__
#include "ps_settings_activity.h"
#include "button_mapping_activity.h"
#include "ui_style.h"
#include "../common.h"
#include <cJSON.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lunar::ui {
namespace {
constexpr int kRumbleStrengths[] = {25, 50, 75, 100};

int rumbleStrengthIndex(int strength) {
    if (strength >= 100) return 3;
    if (strength >= 75) return 2;
    if (strength >= 50) return 1;
    return 0;
}

cJSON* readConfig() {
    cJSON* root = nullptr;
    FILE* f = std::fopen(lunar::get_config_path(), "rb");
    if (f) {
        std::fseek(f, 0, SEEK_END);
        long len = std::ftell(f);
        std::rewind(f);
        if (len > 0 && len < 65536) {
            std::string data(static_cast<size_t>(len), '\0');
            if (std::fread(data.data(), 1, data.size(), f) == data.size()) root = cJSON_Parse(data.c_str());
        }
        std::fclose(f);
    }
    return root ? root : cJSON_CreateObject();
}
bool writeConfig(cJSON* root) {
    char* json = cJSON_Print(root);
    if (!json) return false;
    FILE* f = std::fopen(lunar::get_config_path(), "wb");
    bool ok = f && std::fputs(json, f) >= 0;
    if (f) ok = std::fclose(f) == 0 && ok;
    std::free(json);
    return ok;
}
}

PsSettingsSnapshot loadPsSettings() {
    PsSettingsSnapshot out;
    cJSON* root = readConfig();
    const cJSON* resolution = cJSON_GetObjectItem(root, "ps_resolution");
    if (cJSON_IsNumber(resolution) && resolution->valueint >= 1080) { out.width = 1920; out.height = 1080; }
    const cJSON* bitrate = cJSON_GetObjectItem(root, "ps_bitrate_kbps");
    if (cJSON_IsNumber(bitrate) && bitrate->valueint > 0) out.bitrate_kbps = bitrate->valueint;
    const cJSON* codec = cJSON_GetObjectItem(root, "ps_video_codec");
    if (cJSON_IsString(codec) && codec->valuestring &&
        std::strcmp(codec->valuestring, "hevc") == 0) {
        out.video_codec = stream::VideoCodec::HEVC;
    }
    const cJSON* vibration = cJSON_GetObjectItem(root, "ps_vibration_enabled");
    if (cJSON_IsBool(vibration)) out.vibration_enabled = cJSON_IsTrue(vibration);
    const cJSON* rumble = cJSON_GetObjectItem(root, "ps_rumble_strength_percent");
    if (cJSON_IsNumber(rumble)) {
        out.rumble_strength_percent = std::clamp(rumble->valueint, 0, 100);
    }
    cJSON_Delete(root);
    return out;
}

bool savePsSettings(const PsSettingsSnapshot& settings) {
    cJSON* root = readConfig();
    cJSON_DeleteItemFromObject(root, "ps_resolution");
    cJSON_AddNumberToObject(root, "ps_resolution", settings.height);
    cJSON_DeleteItemFromObject(root, "ps_bitrate_kbps");
    cJSON_AddNumberToObject(root, "ps_bitrate_kbps", settings.bitrate_kbps);
    cJSON_DeleteItemFromObject(root, "ps_video_codec");
    cJSON_AddStringToObject(root, "ps_video_codec",
                            stream::videoCodecName(settings.video_codec));
    cJSON_DeleteItemFromObject(root, "ps_vibration_enabled");
    cJSON_AddBoolToObject(root, "ps_vibration_enabled", settings.vibration_enabled);
    cJSON_DeleteItemFromObject(root, "ps_rumble_strength_percent");
    cJSON_AddNumberToObject(root, "ps_rumble_strength_percent",
                            settings.rumble_strength_percent);
    bool ok = writeConfig(root);
    cJSON_Delete(root);
    return ok;
}

PsSettingsActivity::PsSettingsActivity(PsSettingsSnapshot settings, CompletionCallback completion)
    : settings_(settings), completion_(std::move(completion)) {}

brls::View* PsSettingsActivity::createContentView() {
    const auto& p = uiPalette();
    auto* scroll = new brls::ScrollingFrame();
    scroll->setBackgroundColor(p.background);
    scroll->registerAction(brls::getStr("lunarnx/settings/back_action"), brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool { closeSettings(); return true; });
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(18, 52, 32, 52);
    root->addView(makeSectionHeader(brls::getStr("lunarnx/ps/settings_video_section"), brls::getStr("lunarnx/ps/settings_video_detail")));
    auto* card = makeUiCard(); card->setPadding(4, 8, 4, 8);
    auto* resolution = new brls::SelectorCell();
    resolution->init(brls::getStr("lunarnx/settings/resolution"), {"720p", "1080p"}, settings_.height >= 1080 ? 1 : 0,
        [this](int selected) { settings_.width = selected ? 1920 : 1280; settings_.height = selected ? 1080 : 720; });
    card->addView(resolution);
    auto* bitrate = new brls::SelectorCell();
    bitrate->init(brls::getStr("lunarnx/ps/settings_bitrate"), {"10 Mbps", "20 Mbps", "30 Mbps"}, settings_.bitrate_kbps >= 30000 ? 2 : settings_.bitrate_kbps >= 20000 ? 1 : 0,
        [this](int selected) { settings_.bitrate_kbps = selected >= 2 ? 30000 : selected == 1 ? 20000 : 10000; });
    card->addView(bitrate);
    auto* codec = new brls::SelectorCell();
    codec->init(brls::getStr("lunarnx/ps/settings_codec"),
        {"H.264", "HEVC (H.265)"},
        settings_.video_codec == stream::VideoCodec::HEVC ? 1 : 0,
        [this](int selected) {
            settings_.video_codec = selected == 1
                ? stream::VideoCodec::HEVC : stream::VideoCodec::H264;
        });
    card->addView(codec);
    root->addView(card);
    root->addView(makeSectionHeader(
        brls::getStr("lunarnx/ps/settings_controller_section"),
        brls::getStr("lunarnx/ps/settings_controller_detail")));
    auto* controller_card = makeUiCard(); controller_card->setPadding(4, 8, 4, 8);
    auto* vibration = new brls::BooleanCell();
    vibration->init(brls::getStr("lunarnx/settings/vibration"),
        settings_.vibration_enabled,
        [this](bool enabled) { settings_.vibration_enabled = enabled; });
    controller_card->addView(vibration);
    auto* rumble_strength = new brls::SelectorCell();
    rumble_strength->init(brls::getStr("lunarnx/settings/rumble_strength"),
        {"25%", "50%", "75%", "100%"},
        rumbleStrengthIndex(settings_.rumble_strength_percent),
        [this](int selected) {
            selected = std::clamp(selected, 0, 3);
            settings_.rumble_strength_percent = kRumbleStrengths[selected];
        });
    controller_card->addView(rumble_strength);
    auto* button_mapping = new brls::DetailCell();
    button_mapping->setText(brls::getStr("lunarnx/settings/button_mapping"));
    button_mapping->setDetailText(brls::getStr("lunarnx/settings/button_mapping_detail"));
    button_mapping->registerClickAction([](brls::View*) -> bool {
        brls::Application::pushActivity(new ButtonMappingActivity(
            input::ButtonMappingProfile::PlayStation));
        return true;
    });
    controller_card->addView(button_mapping); root->addView(controller_card);
    auto* done = new brls::Button(); done->setText(brls::getStr("lunarnx/common/done")); stylePrimaryButton(done); done->setMarginTop(28); done->registerClickAction([this](brls::View*) -> bool { closeSettings(); return true; }); root->addView(done);
    scroll->setContentView(root);
    return makeAppFrame(brls::getStr("lunarnx/ps/settings_title"), scroll);
}

void PsSettingsActivity::closeSettings() {
    if (closed_) return;
    closed_ = true;
    savePsSettings(settings_);
    if (completion_) completion_(settings_);
    brls::Application::popActivity(brls::TransitionAnimation::NONE);
}
} // namespace lunar::ui
#endif
