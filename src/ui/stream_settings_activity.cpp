#ifdef __SWITCH__
#include "stream_settings_activity.h"

#include "i18n.h"
#include "ui_style.h"
#include "../common.h"
#include "../diagnostics.h"

#include <cJSON.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace lunar::ui {
namespace {

struct RegionOption {
    const char* label_key;
    const char* ip;
};

const RegionOption kRegions[] = {
    {"lunarnx/settings/region_us1", "4.2.2.2"},
    {"lunarnx/settings/region_us2", "143.244.47.65"},
    {"lunarnx/settings/region_europe", "194.25.0.68"},
    {"lunarnx/settings/region_japan1", "138.199.21.239"},
    {"lunarnx/settings/region_japan2", "210.131.113.123"},
    {"lunarnx/settings/region_korea1", "168.126.63.1"},
    {"lunarnx/settings/region_korea2", "121.125.60.151"},
    {"lunarnx/settings/region_australia", "203.41.44.20"},
    {"lunarnx/settings/region_brazil1", "200.221.11.101"},
    {"lunarnx/settings/region_brazil2", "169.150.198.66"},
    {"lunarnx/settings/region_south_india", "104.211.224.146"},
    {"lunarnx/settings/region_central_india", "104.211.96.159"},
    {"lunarnx/settings/region_poland", "45.134.212.66"},
    {"lunarnx/settings/region_auto", ""},
};

constexpr size_t kRegionCount = sizeof(kRegions) / sizeof(kRegions[0]);

struct LanguageOption {
    const char* value;
    const char* label_key;
};

const LanguageOption kLanguages[] = {
    {"auto", "lunarnx/settings/language_auto"},
    {"en-US", "lunarnx/settings/language_english"},
    {"zh-Hans", "lunarnx/settings/language_simplified"},
    {"zh-Hant", "lunarnx/settings/language_traditional"},
};

constexpr size_t kLanguageCount = sizeof(kLanguages) / sizeof(kLanguages[0]);

struct GameLanguageOption {
    const char* value;
    const char* label;
};

const GameLanguageOption kGameLanguages[] = {
    {"ar-SA", "Arabic (Saudi Arabia)"},
    {"cs-CZ", "Czech"},
    {"da-DK", "Danish"},
    {"de-DE", "Deutsch"},
    {"el-GR", "Greek"},
    {"en-GB", "English (United Kingdom)"},
    {"en-US", "English (United States)"},
    {"es-ES", "Espanol (Espana)"},
    {"es-MX", "Espanol (Mexico)"},
    {"fi-FI", "Finnish"},
    {"fr-FR", "Francais"},
    {"he-IL", "Hebrew"},
    {"hu-HU", "Hungarian"},
    {"it-IT", "Italiano"},
    {"ja-JP", "Japanese"},
    {"ko-KR", "Korean"},
    {"nb-NO", "Norwegian"},
    {"nl-NL", "Dutch"},
    {"pl-PL", "Polish"},
    {"pt-BR", "Portuguese (Brazil)"},
    {"pt-PT", "Portuguese (Portugal)"},
    {"ru-RU", "Russian"},
    {"sk-SK", "Slovak"},
    {"sv-SE", "Swedish"},
    {"tr-TR", "Turkish"},
    {"zh-CN", "Chinese (Simplified)"},
    {"zh-TW", "Chinese (Traditional)"},
};

constexpr size_t kGameLanguageCount =
    sizeof(kGameLanguages) / sizeof(kGameLanguages[0]);

int gameLanguageIndex(const std::string& locale) {
    for (size_t i = 0; i < kGameLanguageCount; ++i) {
        if (locale == kGameLanguages[i].value) return static_cast<int>(i);
    }
    return 6;
}

int regionIndexForIp(const std::string& ip) {
    for (size_t i = 0; i < kRegionCount; ++i) {
        if (ip == kRegions[i].ip) return static_cast<int>(i);
    }
    return 0;
}

int languageIndex(const std::string& language) {
    for (size_t i = 0; i < kLanguageCount; ++i) {
        if (language == kLanguages[i].value) return static_cast<int>(i);
    }
    return 0;
}

cJSON* readConfig() {
    const char* path = lunar::get_config_path();
    cJSON* root = nullptr;
    FILE* input = std::fopen(path, "rb");
    if (input) {
        if (std::fseek(input, 0, SEEK_END) == 0) {
            long len = std::ftell(input);
            if (len > 0) {
                std::rewind(input);
                std::string data(static_cast<size_t>(len), '\0');
                if (std::fread(data.data(), 1, static_cast<size_t>(len), input) ==
                    static_cast<size_t>(len)) {
                    root = cJSON_Parse(data.c_str());
                }
            }
        }
        std::fclose(input);
    }
    if (!root) root = cJSON_CreateObject();
    return root;
}

bool writeConfig(cJSON* root) {
    if (!root) return false;
    char* json = cJSON_Print(root);
    if (!json) return false;
    FILE* output = std::fopen(lunar::get_config_path(), "wb");
    const bool written = output != nullptr;
    if (output) {
        std::fputs(json, output);
        std::fclose(output);
    }
    std::free(json);
    return written;
}

void saveRegion(const char* ip) {
    cJSON* root = readConfig();
    cJSON_DeleteItemFromObject(root, "force_region_ip");
    cJSON_AddStringToObject(root, "force_region_ip", ip ? ip : "");
    writeConfig(root);
    cJSON_Delete(root);
}

constexpr int kRumbleStrengths[] = {25, 50, 75, 100};

int rumbleStrengthIndex(int percent) {
    int closest = 0;
    for (int i = 1; i < 4; ++i) {
        if (std::abs(kRumbleStrengths[i] - percent) <
            std::abs(kRumbleStrengths[closest] - percent)) {
            closest = i;
        }
    }
    return closest;
}

const char* videoBackendConfigValue(stream::VideoBackend backend) {
    switch (backend) {
        case stream::VideoBackend::HardwareCopyOut: return "hardware_copy_out";
        case stream::VideoBackend::Software: return "software";
        case stream::VideoBackend::HardwareZeroCopy:
        default: return "hardware_zero_copy";
    }
}

stream::VideoBackend parseVideoBackend(const char* value) {
    if (value && std::strcmp(value, "hardware_copy_out") == 0) {
        return stream::VideoBackend::HardwareCopyOut;
    }
    if (value && std::strcmp(value, "software") == 0) {
        return stream::VideoBackend::Software;
    }
    if (value && std::strcmp(value, "hardware") == 0) {
        return stream::VideoBackend::HardwareCopyOut;
    }
    return stream::VideoBackend::HardwareZeroCopy;
}

const char* postProcessConfigValue(stream::PostProcessMode mode) {
    switch (mode) {
        case stream::PostProcessMode::Upscale: return "upscale";
        case stream::PostProcessMode::UpscaleRcas: return "upscale_rcas";
        case stream::PostProcessMode::Off:
        default: return "off";
    }
}

stream::PostProcessMode parsePostProcess(const char* value) {
    if (value && std::strcmp(value, "upscale") == 0) {
        return stream::PostProcessMode::Upscale;
    }
    if (value && std::strcmp(value, "upscale_rcas") == 0) {
        return stream::PostProcessMode::UpscaleRcas;
    }
    return stream::PostProcessMode::Off;
}

} // namespace

StreamSettingsSnapshot loadStreamSettings() {
    StreamSettingsSnapshot settings;
    cJSON* root = readConfig();
    if (!root) return settings;

    const cJSON* resolution = cJSON_GetObjectItemCaseSensitive(root, "resolution");
    if (cJSON_IsNumber(resolution) && resolution->valueint >= 1080) {
        settings.width = 1920;
        settings.height = 1080;
    }
    const cJSON* bitrate = cJSON_GetObjectItemCaseSensitive(root, "bitrate");
    if (cJSON_IsNumber(bitrate) && bitrate->valuedouble > 0) {
        settings.bitrate_kbps = static_cast<int>(bitrate->valuedouble * 1000.0);
    } else if (settings.height >= 1080) {
        settings.bitrate_kbps = 20000;
    }
    const cJSON* game_language =
        cJSON_GetObjectItemCaseSensitive(root, "preferred_game_language");
    if (cJSON_IsString(game_language) && game_language->valuestring &&
        game_language->valuestring[0]) {
        settings.preferred_game_language = game_language->valuestring;
    }
    const cJSON* backend = cJSON_GetObjectItemCaseSensitive(root, "video_backend");
    settings.video_backend = parseVideoBackend(
        cJSON_IsString(backend) ? backend->valuestring : nullptr);
    const cJSON* post = cJSON_GetObjectItemCaseSensitive(root, "post_process_mode");
    settings.post_process_mode = parsePostProcess(
        cJSON_IsString(post) ? post->valuestring : nullptr);
    const cJSON* dithering = cJSON_GetObjectItemCaseSensitive(root, "dithering");
    if (cJSON_IsBool(dithering)) settings.dithering_enabled = cJSON_IsTrue(dithering);
    const cJSON* vibration = cJSON_GetObjectItemCaseSensitive(root, "vibration");
    if (cJSON_IsBool(vibration)) settings.vibration_enabled = cJSON_IsTrue(vibration);
    const cJSON* strength =
        cJSON_GetObjectItemCaseSensitive(root, "rumble_strength_percent");
    if (cJSON_IsNumber(strength)) {
        settings.rumble_strength_percent = std::clamp(strength->valueint, 0, 100);
    }
    cJSON_Delete(root);
    return settings;
}

bool saveStreamSettings(const StreamSettingsSnapshot& settings) {
    cJSON* root = readConfig();
    if (!root) return false;
    cJSON_DeleteItemFromObject(root, "resolution");
    cJSON_AddNumberToObject(root, "resolution", settings.height);
    cJSON_DeleteItemFromObject(root, "bitrate");
    cJSON_AddNumberToObject(root, "bitrate", settings.bitrate_kbps / 1000.0);
    cJSON_DeleteItemFromObject(root, "preferred_game_language");
    cJSON_AddStringToObject(root, "preferred_game_language",
                            settings.preferred_game_language.c_str());
    cJSON_DeleteItemFromObject(root, "video_backend");
    cJSON_AddStringToObject(root, "video_backend",
                            videoBackendConfigValue(settings.video_backend));
    cJSON_DeleteItemFromObject(root, "post_process_mode");
    cJSON_AddStringToObject(root, "post_process_mode",
                            postProcessConfigValue(settings.post_process_mode));
    cJSON_DeleteItemFromObject(root, "dithering");
    cJSON_AddBoolToObject(root, "dithering", settings.dithering_enabled);
    cJSON_DeleteItemFromObject(root, "vibration");
    cJSON_AddBoolToObject(root, "vibration", settings.vibration_enabled);
    cJSON_DeleteItemFromObject(root, "rumble_strength_percent");
    cJSON_AddNumberToObject(root, "rumble_strength_percent",
                            settings.rumble_strength_percent);
    const bool written = writeConfig(root);
    cJSON_Delete(root);
    return written;
}

StreamSettingsActivity::StreamSettingsActivity(
    std::shared_ptr<app::StreamController> ctrl,
    StreamSettingsSnapshot settings,
    CompletionCallback completion)
    : ctrl_(std::move(ctrl)),
      settings_(settings),
      completion_(std::move(completion)) {}

brls::View* StreamSettingsActivity::createContentView() {
    const auto& p = uiPalette();
    auto* scroll = new brls::ScrollingFrame();
    scroll->setBackgroundColor(p.background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->registerAction(brls::getStr("lunarnx/settings/back_action"),
        brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            closeSettings();
            return true;
        });

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(28, 64, 40, 64);
    root->setBackgroundColor(p.background);
    scroll->setContentView(root);

    auto* top = new brls::Box(brls::Axis::ROW);
    top->setHeight(72);
    top->setAlignItems(brls::AlignItems::CENTER);

    auto* brand = new brls::Label();
    brand->setText("LUNARNX");
    brand->setFontSize(18);
    brand->setTextColor(p.accent);
    top->addView(brand);

    auto* title = new brls::Label();
    title->setText(brls::getStr("lunarnx/settings/title"));
    title->setFontSize(30);
    title->setTextColor(p.text);
    title->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    title->setGrow(1.0f);
    top->addView(title);
    root->addView(top);

    auto* intro = makeUiCard();
    intro->setMarginBottom(22);
    auto* intro_title = new brls::Label();
    intro_title->setText(brls::getStr("lunarnx/settings/intro_title"));
    intro_title->setFontSize(20);
    intro_title->setTextColor(p.text);
    intro->addView(intro_title);
    intro->addView(makeMutedLabel(brls::getStr("lunarnx/settings/intro_detail"), 14));
    root->addView(intro);

    root->addView(makeSectionHeader(
        brls::getStr("lunarnx/settings/app_section"),
        brls::getStr("lunarnx/settings/app_section_detail")));
    auto* app_card = makeUiCard();
    app_card->setPadding(4, 8, 4, 8);
    std::vector<std::string> language_labels;
    language_labels.reserve(kLanguageCount);
    for (const auto& language : kLanguages) {
        language_labels.emplace_back(brls::getStr(language.label_key));
    }
    auto* language = new brls::SelectorCell();
    language->init(brls::getStr("lunarnx/settings/language"), language_labels,
        languageIndex(getConfiguredLanguage()),
        [](int selected) {
            selected = std::max(0, std::min(selected,
                static_cast<int>(kLanguageCount - 1)));
            if (saveConfiguredLanguage(kLanguages[selected].value)) {
                brls::Application::notify(
                    getLanguageRestartNotice(kLanguages[selected].value));
            } else {
                brls::Application::notify(
                    brls::getStr("lunarnx/settings/language_save_failed"));
            }
        });
    app_card->addView(language);
    root->addView(app_card);

    root->addView(makeSectionHeader(
        brls::getStr("lunarnx/settings/video_section"),
        brls::getStr("lunarnx/settings/video_section_detail")));
    auto* video_card = makeUiCard();
    video_card->setPadding(4, 8, 4, 8);

    auto* resolution = new brls::SelectorCell();
    resolution->init(brls::getStr("lunarnx/settings/resolution"),
        {brls::getStr("lunarnx/settings/resolution_720p"),
         brls::getStr("lunarnx/settings/resolution_1080p"),
         brls::getStr("lunarnx/settings/resolution_1080p_hq")},
        settings_.height >= 1080
            ? (settings_.bitrate_kbps >= 30000 ? 2 : 1)
            : 0,
        [this](int selected) {
            if (selected >= 2) {
                settings_.width = 1920;
                settings_.height = 1080;
                settings_.bitrate_kbps = 30000;
            } else if (selected == 1) {
                settings_.width = 1920;
                settings_.height = 1080;
                settings_.bitrate_kbps = 20000;
            } else {
                settings_.width = 1280;
                settings_.height = 720;
                settings_.bitrate_kbps = 10000;
            }
        });
    video_card->addView(resolution);

    int backend_index = 0;
    if (settings_.video_backend == stream::VideoBackend::HardwareCopyOut) {
        backend_index = 1;
    } else if (settings_.video_backend == stream::VideoBackend::Software) {
        backend_index = 2;
    }
    auto* decoder = new brls::SelectorCell();
    decoder->init(brls::getStr("lunarnx/settings/decoder"),
        {brls::getStr("lunarnx/settings/decoder_hardware"),
         brls::getStr("lunarnx/settings/decoder_copy"),
         brls::getStr("lunarnx/settings/decoder_software")},
        backend_index,
        [this](int selected) {
            if (selected == 1) {
                settings_.video_backend = stream::VideoBackend::HardwareCopyOut;
            } else if (selected == 2) {
                settings_.video_backend = stream::VideoBackend::Software;
            } else {
                settings_.video_backend = stream::VideoBackend::HardwareZeroCopy;
            }
            if (ctrl_) ctrl_->setDefaultVideoBackend(settings_.video_backend);
        });
    video_card->addView(decoder);
    root->addView(video_card);

    root->addView(makeSectionHeader(
        brls::getStr("lunarnx/settings/image_section"),
        brls::getStr("lunarnx/settings/image_section_detail")));
    auto* image_card = makeUiCard();
    image_card->setPadding(4, 8, 4, 8);

    int post_index = 0;
    if (settings_.post_process_mode == stream::PostProcessMode::Upscale) {
        post_index = 1;
    } else if (settings_.post_process_mode == stream::PostProcessMode::UpscaleRcas) {
        post_index = 2;
    }
    auto* post = new brls::SelectorCell();
    post->init(brls::getStr("lunarnx/settings/post_processing"),
        {brls::getStr("lunarnx/settings/off"),
         brls::getStr("lunarnx/settings/upscale"),
         brls::getStr("lunarnx/settings/upscale_rcas")},
        post_index,
        [this](int selected) {
            if (selected == 1) {
                settings_.post_process_mode = stream::PostProcessMode::Upscale;
            } else if (selected == 2) {
                settings_.post_process_mode = stream::PostProcessMode::UpscaleRcas;
            } else {
                settings_.post_process_mode = stream::PostProcessMode::Off;
            }
        });
    image_card->addView(post);

    auto* dithering = new brls::BooleanCell();
    dithering->init(brls::getStr("lunarnx/settings/dithering"),
        settings_.dithering_enabled,
        [this](bool enabled) {
            settings_.dithering_enabled = enabled;
        });
    image_card->addView(dithering);
    root->addView(image_card);

    root->addView(makeSectionHeader(
        brls::getStr("lunarnx/settings/controller_section"),
        brls::getStr("lunarnx/settings/controller_section_detail")));
    auto* controller_card = makeUiCard();
    controller_card->setPadding(4, 8, 4, 8);

    auto* vibration = new brls::BooleanCell();
    vibration->init(brls::getStr("lunarnx/settings/vibration"),
        settings_.vibration_enabled,
        [this](bool enabled) {
            settings_.vibration_enabled = enabled;
            if (ctrl_) ctrl_->setRumbleEnabled(enabled);
        });
    controller_card->addView(vibration);

    auto* rumble_strength = new brls::SelectorCell();
    rumble_strength->init(brls::getStr("lunarnx/settings/rumble_strength"),
        {"25%", "50%", "75%", "100%"},
        rumbleStrengthIndex(settings_.rumble_strength_percent),
        [this](int selected) {
            selected = std::max(0, std::min(selected, 3));
            settings_.rumble_strength_percent = kRumbleStrengths[selected];
            if (ctrl_) {
                ctrl_->setRumbleStrengthPercent(settings_.rumble_strength_percent);
            }
        });
    controller_card->addView(rumble_strength);
    root->addView(controller_card);

    root->addView(makeSectionHeader(
        brls::getStr("lunarnx/settings/region_section"),
        brls::getStr("lunarnx/settings/region_section_detail")));
    auto* cloud_card = makeUiCard();
    cloud_card->setPadding(4, 8, 4, 8);
    std::vector<std::string> game_language_labels;
    game_language_labels.reserve(kGameLanguageCount);
    for (const auto& option : kGameLanguages) {
        game_language_labels.emplace_back(option.label);
    }
    auto* game_language = new brls::SelectorCell();
    game_language->init(brls::getStr("lunarnx/settings/game_language"),
        game_language_labels,
        gameLanguageIndex(settings_.preferred_game_language),
        [this](int selected) {
            selected = std::max(0, std::min(selected,
                static_cast<int>(kGameLanguageCount - 1)));
            settings_.preferred_game_language = kGameLanguages[selected].value;
            if (ctrl_) {
                ctrl_->setPreferredGameLanguage(
                    settings_.preferred_game_language);
            }
        });
    cloud_card->addView(game_language);
    std::vector<std::string> region_labels;
    region_labels.reserve(kRegionCount);
    for (const auto& region : kRegions) {
        region_labels.emplace_back(brls::getStr(region.label_key));
    }
    auto* region = new brls::SelectorCell();
    region->init(brls::getStr("lunarnx/settings/region"), region_labels,
        ctrl_ ? regionIndexForIp(ctrl_->getForceRegionIp()) : 0,
        [this](int selected) {
            selected = std::max(0, std::min(selected,
                static_cast<int>(kRegionCount - 1)));
            if (ctrl_) ctrl_->setForceRegionIp(kRegions[selected].ip);
            saveRegion(kRegions[selected].ip);
            lunar::diagnosticLog("ui-settings", "xCloud region=%s ip=%s",
                                 kRegions[selected].label_key,
                                 kRegions[selected].ip[0]
                                     ? kRegions[selected].ip
                                     : "(native)");
        });
    cloud_card->addView(region);
    root->addView(cloud_card);

    auto* close = new brls::Button();
    close->setText(brls::getStr("lunarnx/common/done"));
    stylePrimaryButton(close);
    close->setMarginTop(28);
    close->registerClickAction([this](brls::View*) -> bool {
        closeSettings();
        return true;
    });
    root->addView(close);

    auto* hint = makeMutedLabel(brls::getStr("lunarnx/settings/footer_back"), 13);
    hint->setHeight(34);
    hint->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    root->addView(hint);
    return scroll;
}

void StreamSettingsActivity::closeSettings() {
    if (closed_) return;
    closed_ = true;
    saveStreamSettings(settings_);
    if (completion_) completion_(settings_);
    brls::Application::popActivity(brls::TransitionAnimation::NONE);
}

} // namespace lunar::ui
#endif
