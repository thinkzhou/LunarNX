#ifdef __SWITCH__
#include "platform_activity.h"
#include "auth_activity.h"
#include "main_activity.h"
#include "ps_activity.h"
#include "ui_style.h"
#include "../common.h"
#include "../diagnostics.h"

#include <functional>

namespace lunar::ui {
namespace {

brls::Box* makePlatformRow(const std::string& title,
                           const std::string& detail,
                           const std::function<void()>& open) {
    const auto& p = uiPalette();
    auto* row = makeUiCard(brls::Axis::ROW);
    row->setWidth(760);
    row->setHeight(132);
    row->setPadding(22, 26, 22, 26);
    row->setAlignItems(brls::AlignItems::CENTER);

    auto* copy = new brls::Box(brls::Axis::COLUMN);
    copy->setGrow(1.0f);
    copy->setPadding(4, 12, 4, 0);
    auto* label = new brls::Label();
    label->setText(title);
    label->setFontSize(24);
    label->setTextColor(p.text);
    copy->addView(label);
    auto* description = makeMutedLabel(detail, 14);
    description->setMarginTop(8);
    copy->addView(description);
    row->addView(copy);

    auto* button = new brls::Button();
    button->setWidth(180);
    button->setHeight(64);
    button->setText(brls::getStr("lunarnx/platform/open"));
    stylePrimaryButton(button);
    button->registerClickAction([open](brls::View*) -> bool {
        open();
        return true;
    });
    row->addView(button);
    return row;
}

} // namespace

PlatformActivity::PlatformActivity() = default;

brls::View* PlatformActivity::createContentView() {
    diagnosticLog("ui-platform", "createContentView begin");
    const auto& p = uiPalette();

    auto* scroll = new brls::ScrollingFrame();
    scroll->setBackgroundColor(p.background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(40, 60, 40, 20);
    root->setAlignItems(brls::AlignItems::CENTER);

    // Title
    auto* title = new brls::Label();
    title->setText("LunarNX");
    title->setFontSize(32);
    title->setTextColor(p.text);
    title->setMargins(0, 20, 0, 30);
    root->addView(title);

    // Subtitle
    auto* subtitle = new brls::Label();
    subtitle->setText(brls::getStr("lunarnx/platform/subtitle"));
    subtitle->setFontSize(16);
    subtitle->setTextColor(p.text_muted);
    subtitle->setMargins(0, 0, 0, 40);
    root->addView(subtitle);

    auto* xbox_card = makePlatformRow(brls::getStr("lunarnx/platform/xbox_title"),
        brls::getStr("lunarnx/platform/xbox_desc"), [this]() {
        diagnosticLog("ui-platform", "Xbox Open clicked");
        openXbox();
    });
    xbox_card->setMarginBottom(18);
    root->addView(xbox_card);

    auto* ps_card = makePlatformRow(brls::getStr("lunarnx/platform/ps_title"),
        brls::getStr("lunarnx/platform/ps_desc"), [this]() {
        diagnosticLog("ui-platform", "PlayStation Open clicked");
        openPlayStation();
    });
    root->addView(ps_card);

    scroll->registerAction("Exit", brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            const auto now = std::chrono::steady_clock::now();
            if (now < exit_navigation_ready_at_) return true;
            brls::Application::quit();
            return true;
        });

    scroll->setContentView(root);
    diagnosticLog("ui-platform", "createContentView complete");
    return scroll;
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

} // namespace lunar::ui
#endif
