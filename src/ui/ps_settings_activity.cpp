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
    bool ok = writeConfig(root);
    cJSON_Delete(root);
    return ok;
}

PsSettingsActivity::PsSettingsActivity(PsSettingsSnapshot settings, CompletionCallback completion)
    : settings_(settings), completion_(std::move(completion)) {}

brls::View* PsSettingsActivity::createContentView() {
    const auto& p = uiPalette();

    auto* page = new brls::Box(brls::Axis::COLUMN);
    page->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
    page->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
    page->setBackgroundColor(p.background);
    page->registerAction(brls::getStr("lunarnx/settings/back_action"),
        brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            closeSettings();
            return true;
        });

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setBackgroundColor(p.background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(26, 48, 30, 48);

    auto* header = new brls::Box(brls::Axis::ROW);
    header->setHeight(76);
    header->setAlignItems(brls::AlignItems::CENTER);
    auto* brand = new brls::Label();
    brand->setText("LUNARNX");
    brand->setFontSize(26);
    brand->setTextColor(p.accent);
    brand->setGrow(1.0f);
    header->addView(brand);
    auto* title = new brls::Label();
    title->setText(brls::getStr("lunarnx/ps/settings_title"));
    title->setFontSize(30);
    title->setTextColor(p.text);
    title->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    header->addView(title);
    root->addView(header);

    root->addView(makeSectionHeader(
        brls::getStr("lunarnx/ps/settings_video_section"),
        brls::getStr("lunarnx/ps/settings_video_detail")));
    auto* card = makeUiCard();
    card->setPadding(4, 8, 4, 8);
    auto* resolution = new brls::SelectorCell();
    resolution->init(brls::getStr("lunarnx/settings/resolution"),
        {"720p", "1080p"}, settings_.height >= 1080 ? 1 : 0,
        [this](int selected) {
            settings_.width = selected ? 1920 : 1280;
            settings_.height = selected ? 1080 : 720;
        });
    card->addView(resolution);
    auto* bitrate = new brls::SelectorCell();
    bitrate->init(brls::getStr("lunarnx/ps/settings_bitrate"),
        {"10 Mbps", "20 Mbps", "30 Mbps"},
        settings_.bitrate_kbps >= 30000 ? 2
            : settings_.bitrate_kbps >= 20000 ? 1 : 0,
        [this](int selected) {
            settings_.bitrate_kbps = selected >= 2 ? 30000
                : selected == 1 ? 20000 : 10000;
        });
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
    auto* controller_card = makeUiCard();
    controller_card->setPadding(4, 8, 4, 8);
    auto* button_mapping = new brls::DetailCell();
    button_mapping->setText(brls::getStr("lunarnx/settings/button_mapping"));
    button_mapping->setDetailText(brls::getStr("lunarnx/settings/button_mapping_detail"));
    button_mapping->registerClickAction([](brls::View*) -> bool {
        brls::Application::pushActivity(new ButtonMappingActivity(
            input::ButtonMappingProfile::PlayStation));
        return true;
    });
    controller_card->addView(button_mapping);
    root->addView(controller_card);

    auto* action_row = new brls::Box(brls::Axis::ROW);
    action_row->setHeight(72);
    action_row->setJustifyContent(brls::JustifyContent::FLEX_END);
    action_row->setAlignItems(brls::AlignItems::CENTER);
    auto* done = new brls::Button();
    done->setWidth(170);
    done->setText(brls::getStr("lunarnx/common/done"));
    stylePrimaryButton(done);
    done->registerClickAction([this](brls::View*) -> bool {
        closeSettings();
        return true;
    });
    action_row->addView(done);
    root->addView(action_row);

    scroll->setContentView(root);
    page->addView(scroll);
    auto* footer = makeHintBar(brls::getStr("lunarnx/common/confirm"),
                               brls::getStr("lunarnx/common/back"));
    footer->setWidthPercentage(100.0f);
    footer->setBackgroundColor(p.surface);
    page->addView(footer);
    return page;
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
