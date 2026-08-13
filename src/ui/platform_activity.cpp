#ifdef __SWITCH__
#include "platform_activity.h"
#include "auth_activity.h"
#include "main_activity.h"
#include "ps_activity.h"
#include "about_activity.h"
#include "stream_settings_activity.h"
#include "ui_style.h"
#include "../common.h"
#include "../diagnostics.h"

#include <cstdio>
#include <functional>
#include <string>

namespace lunar::ui {
namespace {

std::string formatResult(Result rc) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%u / 0x%X",
                  static_cast<unsigned int>(rc), static_cast<unsigned int>(rc));
    return buffer;
}

brls::Button* makePlatformTile(const std::string& title,
                               const std::string& detail,
                               const std::string& account_state,
                               const std::string& console_type,
                               const std::function<void()>& open) {
    const auto& p = uiPalette();
    auto* tile = new brls::Button();
    tile->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    tile->setWidth(330);
    tile->setHeight(350);
    tile->setPadding(20, 22, 20, 22);
    tile->setBackgroundColor(p.card);
    tile->setCornerRadius(4);
    tile->setHighlightCornerRadius(4);
    tile->setAxis(brls::Axis::COLUMN);
    tile->setAlignItems(brls::AlignItems::CENTER);

    auto* glyph = new ConsoleGlyphView(console_type, true);
    glyph->setWidth(190);
    glyph->setHeight(190);
    tile->addView(glyph);

    auto* label = new brls::Label();
    label->setText(title);
    label->setFontSize(27);
    label->setTextColor(p.text);
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    label->setHeight(42);
    tile->addView(label);
    auto* description = makeMutedLabel(detail, 14);
    description->setWidth(280);
    description->setHeight(54);
    description->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    tile->addView(description);
    auto* state = new brls::Label();
    state->setText(account_state);
    state->setFontSize(12);
    state->setTextColor(p.accent);
    state->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    state->setHeight(24);
    tile->addView(state);
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

    auto* workspace = new brls::Box(brls::Axis::ROW);
    workspace->setBackgroundColor(p.background);

    auto* sidebar = new brls::Box(brls::Axis::COLUMN);
    sidebar->setWidth(286);
    sidebar->setPadding(30, 28, 30, 34);
    sidebar->setBackgroundColor(p.surface);
    sidebar->addView(makeSidebarButton(
        brls::getStr("lunarnx/platform/subtitle"), true));

    auto* settings = makeSidebarButton(brls::getStr("lunarnx/common/settings"));
    settings->registerClickAction([](brls::View*) -> bool {
        brls::Application::pushActivity(
            new StreamSettingsActivity(nullptr, loadStreamSettings(), {}),
            brls::TransitionAnimation::NONE);
        return true;
    });
    sidebar->addView(settings);

    auto* browser_test = makeSidebarButton("Browser Test: Baidu");
    browser_test->registerClickAction([this](brls::View*) -> bool {
        openBrowserDiagnostic();
        return true;
    });
    sidebar->addView(browser_test);

    auto* about = makeSidebarButton(brls::getStr("lunarnx/common/about"));
    about->registerClickAction([](brls::View*) -> bool {
        brls::Application::pushActivity(
            new AboutActivity(), brls::TransitionAnimation::NONE);
        return true;
    });
    sidebar->addView(about);
    workspace->addView(sidebar);

    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setGrow(1.0f);
    content->setPadding(30, 48, 28, 48);
    content->addView(makePageHeading(
        brls::getStr("lunarnx/platform/subtitle"),
        "Xbox and PlayStation settings remain separate"));

    auto* platforms = new brls::Box(brls::Axis::ROW);
    platforms->setGrow(1.0f);
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
    ps_card->setMarginLeft(22);
    platforms->addView(ps_card);
    content->addView(platforms);
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

void PlatformActivity::openBrowserDiagnostic() {
    constexpr const char* kDiagnosticUrl = "https://www.baidu.com";
    const AppletType applet_type = appletGetAppletType();
    diagnosticLog("ui-platform-browser", "begin applet_type=%d url=%s",
                  static_cast<int>(applet_type), kDiagnosticUrl);

    if (applet_type != AppletType_Application &&
        applet_type != AppletType_SystemApplication) {
        const std::string message = "Browser test requires Application Mode (applet type " +
            std::to_string(static_cast<int>(applet_type)) + ")";
        diagnosticLog("ui-platform-browser", "%s", message.c_str());
        brls::Application::notify(message);
        return;
    }

    WebCommonConfig config{};
    Result rc = webPageCreate(&config, kDiagnosticUrl);
    if (R_FAILED(rc)) {
        const std::string message = "Baidu browser create failed: " + formatResult(rc);
        diagnosticLog("ui-platform-browser", "webPageCreate failed rc=%u hex=0x%x",
                      static_cast<unsigned int>(rc), static_cast<unsigned int>(rc));
        brls::Application::notify(message);
        return;
    }

    WebCommonReply reply{};
    rc = webConfigShow(&config, &reply);
    if (R_FAILED(rc)) {
        const std::string message = "Baidu browser show failed: " + formatResult(rc);
        diagnosticLog("ui-platform-browser", "webConfigShow failed rc=%u hex=0x%x",
                      static_cast<unsigned int>(rc), static_cast<unsigned int>(rc));
        brls::Application::notify(message);
        return;
    }

    WebExitReason reason = WebExitReason_UnknownE;
    const Result reason_rc = webReplyGetExitReason(&reply, &reason);
    diagnosticLog("ui-platform-browser", "complete reason_rc=%u reason=%d",
                  static_cast<unsigned int>(reason_rc), static_cast<int>(reason));
    if (R_FAILED(reason_rc)) {
        brls::Application::notify("Baidu browser closed; exit reason unavailable (" +
                                  formatResult(reason_rc) + ")");
        return;
    }
    brls::Application::notify("Baidu browser closed; exit reason " +
                              std::to_string(static_cast<int>(reason)));
}

} // namespace lunar::ui
#endif
