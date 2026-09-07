#include "steam_settings_activity.h"
#if defined(__SWITCH__) && LUNARNX_STEAMLINK
#include "ui_style.h"
#include "button_mapping_activity.h"
#include "../common.h"
#include "../diagnostics.h"
#include "../steamlink/settings_file.h"
#include <cJSON.h>
#include <cstdio>
#include <cstring>
namespace lunar::ui {
namespace {
std::string settingsPath() { return std::string(lunar::get_config_path()) + ".steam-input"; }
int mode(cJSON* root, const char* key, int fallback) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) && item->valuedouble == item->valueint &&
        item->valueint >= 0 && item->valueint <= 2 ? item->valueint : fallback;
}
}
SteamInputSettings loadSteamInputSettings() {
    SteamInputSettings settings;
    FILE* file = steamlink::openSettingsFile(settingsPath());
    if (!file) return settings;
    char data[1025]{};
    const size_t count = std::fread(data, 1, 1024, file);
    const bool valid = !std::ferror(file) && std::feof(file);
    std::fclose(file);
    if (!valid || !count) return settings;
    cJSON* root = cJSON_Parse(data);
    if (root) {
        settings.touch = static_cast<steamlink::TouchMode>(mode(root, "touch", 1));
        settings.gyro = static_cast<steamlink::GyroMode>(mode(root, "gyro", 1));
        cJSON_Delete(root);
    }
    return settings;
}
bool saveSteamInputSettings(const SteamInputSettings& settings) {
    const auto path = settingsPath();
    const auto temporary = path + ".tmp";
    ensureDiagnosticLogDirectory();
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) {
        diagnosticLog("steam-settings", "save failed stage=open errno=%d", errno);
        return false;
    }
    const bool written = std::fprintf(file, "{\"touch\":%d,\"gyro\":%d}\n",
        int(settings.touch), int(settings.gyro)) > 0;
    const int write_error = written ? 0 : errno;
    const bool closed = std::fclose(file) == 0;
    const int close_error = closed ? 0 : errno;
    const bool ok = written && closed && steamlink::commitSettingsFile(temporary, path);
    const int error = !written ? write_error : !closed ? close_error : errno;
    if (!ok) diagnosticLog("steam-settings", "save failed stage=%s errno=%d (%s)",
        !written ? "write" : !closed ? "close" : "commit", error, std::strerror(error));
    else diagnosticLog("steam-settings", "saved touch=%d gyro=%d", int(settings.touch), int(settings.gyro));
    if (!ok) std::remove(temporary.c_str());
    return ok;
}
brls::View* SteamSettingsActivity::createContentView() {
    auto* scroll = new brls::ScrollingFrame();
    scroll->setBackgroundColor(uiPalette().background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->registerAction(brls::getStr("lunarnx/common/back"), brls::ControllerButton::BUTTON_B,
        [this](brls::View*) { closeSettings(); return true; });
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(20, 52, 28, 52);
    scroll->setContentView(root);
    root->addView(makeSectionHeader(brls::getStr("lunarnx/steam_ui/input_title"),
        brls::getStr("lunarnx/steam_ui/settings_help")));
    auto* card = makeUiCard();
    auto* touch = new brls::SelectorCell();
    touch->init(brls::getStr("lunarnx/steam_ui/touch"),
        {brls::getStr("lunarnx/steam_ui/off"), brls::getStr("lunarnx/steam_ui/trackpad"),
         brls::getStr("lunarnx/steam_ui/absolute")}, int(settings_.touch),
        [this](int selected) { if (selected >= 0 && selected <= 2)
            settings_.touch = static_cast<steamlink::TouchMode>(selected); });
    card->addView(touch);
    auto* gyro = new brls::SelectorCell();
    gyro->init(brls::getStr("lunarnx/steam_ui/gyro"),
        {brls::getStr("lunarnx/steam_ui/off"), brls::getStr("lunarnx/steam_ui/native"),
         brls::getStr("lunarnx/steam_ui/mouse")}, int(settings_.gyro),
        [this](int selected) { if (selected >= 0 && selected <= 2)
            settings_.gyro = static_cast<steamlink::GyroMode>(selected); });
    card->addView(gyro);
    auto* mapping = new brls::DetailCell();
    mapping->setText(brls::getStr("lunarnx/steam_ui/mapping_title"));
    mapping->setFocusable(true);
    mapping->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new ButtonMappingActivity(input::ButtonMappingProfile::Steam),
            brls::TransitionAnimation::NONE); return true;
    });
    card->addView(mapping);
    root->addView(card);
    auto* help = makeMutedLabel(brls::getStr("lunarnx/steam_ui/gesture_help"), 16);
    help->setSingleLine(false);
    help->setIsWrapping(true); help->setHeight(132); help->setMarginTop(20);
    root->addView(help);
    auto* done = new brls::Button();
    done->setText(brls::getStr("lunarnx/steam_ui/save"));
    done->setHeight(54); stylePrimaryButton(done);
    done->registerClickAction([this](brls::View*) { closeSettings(); return true; });
    root->addView(done);
    return makeAppFrame(brls::getStr("lunarnx/steam_ui/settings_title"), scroll);
}
void SteamSettingsActivity::closeSettings() {
    if (!saveSteamInputSettings(settings_)) {
        brls::Application::notify(brls::getStr("lunarnx/steam_ui/save_failed"));
        return;
    }
    if (completion_) completion_(settings_);
    brls::Application::popActivity(brls::TransitionAnimation::NONE);
}
}
#endif
