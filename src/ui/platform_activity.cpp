#ifdef __SWITCH__
#include "platform_activity.h"
#include "auth_activity.h"
#include "main_activity.h"
#include "ps_activity.h"
#include "about_activity.h"
#include "stream_settings_activity.h"
#include "dev_tools_activity.h"
#include "ui_style.h"
#include "../common.h"
#include "../diagnostics.h"

#include <cstdio>
#include <functional>
#include <string>

namespace lunar::ui {
namespace {

brls::Button* makePlatformTile(const std::string& title,
                               const std::string& detail,
                               const std::string& account_state,
                               const std::string& console_type,
                               const std::function<void()>& open) {
    const auto& p = uiPalette();
    auto* tile = new brls::Button();
    tile->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    tile->setWidth(500);
    tile->setHeight(220);
    tile->setPadding(24, 28, 24, 28);
    tile->setBackgroundColor(p.card);
    tile->setBorderThickness(1);
    tile->setBorderColor(p.border);
    tile->setCornerRadius(8);
    tile->setHighlightCornerRadius(10);
    tile->setAxis(brls::Axis::ROW);
    tile->setAlignItems(brls::AlignItems::CENTER);

    auto* glyph = new ConsoleGlyphView(console_type, true);
    glyph->setWidth(150);
    glyph->setHeight(150);
    tile->addView(glyph);

    auto* copy = new brls::Box(brls::Axis::COLUMN);
    copy->setGrow(1.0f);
    copy->setPadding(12, 0, 12, 26);
    auto* label = new brls::Label();
    label->setText(title);
    label->setFontSize(28);
    label->setTextColor(p.text);
    label->setHeight(42);
    copy->addView(label);
    auto* description = makeMutedLabel(detail, 14);
    description->setHeight(48);
    description->setIsWrapping(true);
    copy->addView(description);
    auto* state = new brls::Label();
    state->setText(account_state);
    state->setFontSize(13);
    state->setTextColor(p.accent);
    state->setHeight(28);
    copy->addView(state);
    tile->addView(copy);
    tile->registerClickAction([open](brls::View*) -> bool {
        open();
        return true;
    });
    return tile;
}

brls::Button* makeUtilityTile(const std::string& title,
                              const std::string& detail,
                              const std::function<void()>& open) {
    const auto& p = uiPalette();
    auto* tile = new brls::Button();
    tile->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    tile->setWidth(1024);
    tile->setHeight(72);
    tile->setPadding(10, 22, 10, 22);
    tile->setBackgroundColor(p.surface_alt);
    tile->setBorderThickness(1);
    tile->setBorderColor(p.border);
    tile->setCornerRadius(8);
    tile->setHighlightCornerRadius(10);
    tile->setAxis(brls::Axis::ROW);
    tile->setAlignItems(brls::AlignItems::CENTER);

    auto* label = new brls::Label();
    label->setText(title);
    label->setFontSize(18);
    label->setTextColor(p.text);
    label->setWidth(240);
    tile->addView(label);
    auto* description = makeMutedLabel(detail, 13);
    description->setGrow(1.0f);
    description->setSingleLine(true);
    tile->addView(description);
    tile->registerClickAction([open](brls::View*) -> bool {
        open();
        return true;
    });
    return tile;
}

bool savedFileExists(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}

} // namespace

PlatformActivity::PlatformActivity() = default;

brls::View* PlatformActivity::createContentView() {
    diagnosticLog("ui-platform", "createContentView begin");
    const auto& p = uiPalette();

    auto* workspace = new brls::Box(brls::Axis::COLUMN);
    workspace->setBackgroundColor(p.background);
    workspace->setPadding(28, 48, 24, 48);
    workspace->registerAction(brls::getStr("lunarnx/common/settings"),
        brls::ControllerButton::BUTTON_X, [](brls::View*) -> bool {
        brls::Application::pushActivity(
            new StreamSettingsActivity(nullptr, loadStreamSettings(), {},
                StreamSettingsScope::Global),
            brls::TransitionAnimation::NONE);
        return true;
    });
    workspace->registerAction(brls::getStr("lunarnx/common/about"),
        brls::ControllerButton::BUTTON_Y, [](brls::View*) -> bool {
        brls::Application::pushActivity(
            new AboutActivity(), brls::TransitionAnimation::NONE);
        return true;
    });

    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setGrow(1.0f);
    content->setAlignItems(brls::AlignItems::CENTER);
    content->setJustifyContent(brls::JustifyContent::CENTER);
    content->addView(makePageHeading(
        brls::getStr("lunarnx/platform/subtitle")));

    auto* platforms = new brls::Box(brls::Axis::ROW);
    platforms->setHeight(250);
    platforms->setAlignItems(brls::AlignItems::CENTER);
    platforms->setJustifyContent(brls::JustifyContent::CENTER);
    const bool xbox_saved = savedFileExists(lunar::get_token_path());
    auto* xbox_card = makePlatformTile(brls::getStr("lunarnx/platform/xbox_title"),
        brls::getStr("lunarnx/platform/xbox_desc"),
        brls::getStr(xbox_saved
            ? "lunarnx/common/signed_in_microsoft"
            : "lunarnx/common/not_signed_in"),
        "SeriesX", [this]() {
        diagnosticLog("ui-platform", "Xbox Open clicked");
        openXbox();
    });
    platforms->addView(xbox_card);

    const bool ps_saved = savedFileExists(lunar::get_ps_credentials_path()) ||
        savedFileExists(lunar::get_psn_token_path());
    auto* ps_card = makePlatformTile(brls::getStr("lunarnx/platform/ps_title"),
        brls::getStr("lunarnx/platform/ps_desc"),
        brls::getStr(ps_saved
            ? "lunarnx/ps/signed_in"
            : "lunarnx/common/not_signed_in"),
        "PS5", [this]() {
        diagnosticLog("ui-platform", "PlayStation Open clicked");
        openPlayStation();
    });
    ps_card->setMarginLeft(24);
    platforms->addView(ps_card);
    content->addView(platforms);
    content->addView(makeUtilityTile(
        brls::getStr("lunarnx/dev/title"),
        brls::getStr("lunarnx/dev/entry_desc"),
        [this]() { openDevTools(); }));
    workspace->addView(content);

    workspace->registerAction("Exit", brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            const auto now = std::chrono::steady_clock::now();
            if (now < exit_navigation_ready_at_) return true;
            brls::Application::quit();
            return true;
        });

    diagnosticLog("ui-platform", "createContentView complete");
    return makeAppFrame("LunarNX", workspace);
}

void PlatformActivity::onResume() {
    // Prevent a held Back press from closing a child activity and quitting the
    // whole application on the next controller-repeat frame.
    exit_navigation_ready_at_ = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(750);
}

void PlatformActivity::openXbox() {
    diagnosticLog("ui-platform", "Xbox navigation begin");
    auto ctrl = AuthActivity::createConfiguredController();
    const bool saved = ctrl->isMockMode() ||
        (ctrl->loadTokens(lunar::get_token_path()) && ctrl->hasCredentials());
    if (saved) {
        ctrl->loadConsoleCache();
        brls::Application::pushActivity(
            new MainActivity(ctrl), brls::TransitionAnimation::NONE);
    } else {
        brls::Application::pushActivity(
            new AuthActivity(ctrl), brls::TransitionAnimation::NONE);
    }
    diagnosticLog("ui-platform", "Xbox navigation complete");
}

void PlatformActivity::openPlayStation() {
    diagnosticLog("ui-platform", "PlayStation navigation begin");
    auto* ps_activity = new PsActivity();
    diagnosticLog("ui-platform", "PsActivity constructed");
    brls::Application::pushActivity(
        ps_activity,
        brls::TransitionAnimation::NONE);
    diagnosticLog("ui-platform", "PlayStation navigation complete");
}

void PlatformActivity::openDevTools() {
    diagnosticLog("ui-platform", "Dev tools navigation begin");
    brls::Application::pushActivity(
        new DevToolsActivity(), brls::TransitionAnimation::NONE);
}

} // namespace lunar::ui
#endif
