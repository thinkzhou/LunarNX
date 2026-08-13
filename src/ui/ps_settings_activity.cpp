#ifdef __SWITCH__
#include "ps_settings_activity.h"
#include "ui_style.h"
#include "../common.h"
#include <cJSON.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace lunar::ui {
namespace {
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
    cJSON_Delete(root);
    return out;
}

bool savePsSettings(const PsSettingsSnapshot& settings) {
    cJSON* root = readConfig();
    cJSON_DeleteItemFromObject(root, "ps_resolution");
    cJSON_AddNumberToObject(root, "ps_resolution", settings.height);
    cJSON_DeleteItemFromObject(root, "ps_bitrate_kbps");
    cJSON_AddNumberToObject(root, "ps_bitrate_kbps", settings.bitrate_kbps);
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
    root->setPadding(24, 64, 40, 64);
    auto* card = makeFlatSection(
        brls::getStr("lunarnx/ps/settings_video_section"),
        brls::getStr("lunarnx/ps/settings_video_detail"));
    auto* resolution = new brls::SelectorCell();
    resolution->init(brls::getStr("lunarnx/settings/resolution"), {"720p", "1080p"}, settings_.height >= 1080 ? 1 : 0,
        [this](int selected) { settings_.width = selected ? 1920 : 1280; settings_.height = selected ? 1080 : 720; });
    addFlatRow(card, resolution);
    auto* bitrate = new brls::SelectorCell();
    bitrate->init(brls::getStr("lunarnx/ps/settings_bitrate"), {"10 Mbps", "20 Mbps", "30 Mbps"}, settings_.bitrate_kbps >= 30000 ? 2 : settings_.bitrate_kbps >= 20000 ? 1 : 0,
        [this](int selected) { settings_.bitrate_kbps = selected >= 2 ? 30000 : selected == 1 ? 20000 : 10000; });
    addFlatRow(card, bitrate);
    root->addView(card);
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
