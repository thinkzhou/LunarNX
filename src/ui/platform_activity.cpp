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

// Draws a recognizable platform logo: the Xbox sphere-and-X mark, or the
// PlayStation "PS" tile. Rendered with NanoVG so no binary assets are needed.
class PlatformLogoView : public brls::View {
public:
    explicit PlatformLogoView(std::string platform)
        : platform_(std::move(platform)) {
        setWidth(64);
        setHeight(64);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        (void)style;
        (void)ctx;
        const auto& p = uiPalette();
        const float cx = x + width * 0.5f;
        const float cy = y + height * 0.5f;
        const float r = 28.0f;

        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r);
        nvgFillColor(vg, p.background);
        nvgFill(vg);

        if (platform_ == "xbox") {
            const float body_w = 31.0f;
            const float body_h = 17.0f;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - body_w * 0.5f, cy - body_h * 0.5f,
                           body_w, body_h, 3);
            nvgStrokeWidth(vg, 3);
            nvgStrokeColor(vg, p.xbox_green);
            nvgStroke(vg);

            nvgBeginPath(vg);
            nvgMoveTo(vg, cx - 10, cy);
            nvgLineTo(vg, cx - 2, cy);
            nvgMoveTo(vg, cx - 6, cy - 4);
            nvgLineTo(vg, cx - 6, cy + 4);
            nvgStroke(vg);

            nvgBeginPath(vg);
            nvgCircle(vg, cx + 7, cy - 2, 2);
            nvgCircle(vg, cx + 12, cy + 2, 2);
            nvgFillColor(vg, p.xbox_green);
            nvgFill(vg);
        } else {
            nvgStrokeWidth(vg, 3);
            nvgStrokeColor(vg, p.ps_blue);
            nvgLineCap(vg, NVG_ROUND);
            nvgBeginPath(vg);
            nvgMoveTo(vg, cx, cy - 15);
            nvgLineTo(vg, cx, cy + 15);
            nvgMoveTo(vg, cx - 15, cy);
            nvgLineTo(vg, cx + 15, cy);
            nvgStroke(vg);

            for (const auto& point : {std::pair<float, float>{cx, cy - 15},
                                      {cx + 15, cy}, {cx, cy + 15},
                                      {cx - 15, cy}}) {
                nvgBeginPath(vg);
                nvgRect(vg, point.first - 4, point.second - 4, 8, 8);
                nvgStroke(vg);
            }
        }
    }

private:
    std::string platform_;
};

brls::Box* makePlatformRow(const std::string& platform,
                           const std::string& title,
                           const std::string& detail,
                           const std::function<void()>& open) {
    const auto& p = uiPalette();
    auto* card = makeUiCard(brls::Axis::COLUMN);
    card->setWidth(360);
    card->setHeight(285);
    card->setPadding(24, 24, 22, 24);
    card->addView(new PlatformLogoView(platform));

    auto* label = new brls::Label();
    label->setText(title);
    label->setFontSize(24);
    label->setTextColor(p.text);
    label->setMarginTop(14);
    card->addView(label);

    auto* description = makeMutedLabel(detail, 14);
    description->setMarginTop(4);
    card->addView(description);

    auto* spacer = new brls::Box();
    spacer->setGrow(1.0f);
    card->addView(spacer);

    auto* action_row = new brls::Box(brls::Axis::ROW);
    action_row->setJustifyContent(brls::JustifyContent::FLEX_END);
    auto* action = new brls::Button();
    action->setWidth(104);
    action->setText(brls::getStr("lunarnx/platform/open"));
    stylePrimaryButton(action);
    action->registerClickAction([open](brls::View*) -> bool {
        open();
        return true;
    });
    action_row->addView(action);
    card->addView(action_row);
    return card;
}

} // namespace

PlatformActivity::PlatformActivity() = default;

brls::View* PlatformActivity::createContentView() {
    diagnosticLog("ui-platform", "createContentView begin");
    const auto& p = uiPalette();

    auto* page = new brls::Box(brls::Axis::COLUMN);
    page->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
    page->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
    page->setBackgroundColor(p.background);

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setGrow(1.0f);
    root->setPadding(42, 0, 20, 0);
    root->setAlignItems(brls::AlignItems::CENTER);

    auto* heading = new brls::Box(brls::Axis::COLUMN);
    heading->setHeight(84);
    heading->setAlignItems(brls::AlignItems::CENTER);

    auto* title = new brls::Label();
    title->setText("LUNARNX");
    title->setFontSize(28);
    title->setTextColor(p.accent);
    heading->addView(title);

    auto* subtitle = new brls::Label();
    subtitle->setText(brls::getStr("lunarnx/platform/subtitle"));
    subtitle->setFontSize(16);
    subtitle->setTextColor(p.text_muted);
    subtitle->setMarginTop(8);
    heading->addView(subtitle);
    root->addView(heading);

    auto* heading_gap = new brls::Box();
    heading_gap->setHeight(110);
    root->addView(heading_gap);

    auto* cards = new brls::Box(brls::Axis::ROW);
    cards->setAlignItems(brls::AlignItems::CENTER);

    auto* xbox_card = makePlatformRow("xbox",
        brls::getStr("lunarnx/platform/xbox_title"),
        brls::getStr("lunarnx/platform/xbox_desc"), [this]() {
        diagnosticLog("ui-platform", "Xbox Open clicked");
        openXbox();
    });
    xbox_card->setMarginRight(20);
    cards->addView(xbox_card);

    auto* ps_card = makePlatformRow("ps",
        brls::getStr("lunarnx/platform/ps_title"),
        brls::getStr("lunarnx/platform/ps_desc"), [this]() {
        diagnosticLog("ui-platform", "PlayStation Open clicked");
        openPlayStation();
    });
    cards->addView(ps_card);

    root->addView(cards);
    page->addView(root);
    auto* footer = makeHintBar("", brls::getStr("lunarnx/common/exit"));
    footer->setWidthPercentage(100.0f);
    footer->setBackgroundColor(p.surface);
    auto* footer_spacer = new brls::Box();
    footer_spacer->setGrow(1.0f);
    footer->addView(footer_spacer);
    auto* version = makeMutedLabel("LUNARNX v" LUNARNX_VERSION, 13);
    footer->addView(version);
    page->addView(footer);

    page->registerAction("Exit", brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            const auto now = std::chrono::steady_clock::now();
            if (now < exit_navigation_ready_at_) return true;
            brls::Application::quit();
            return true;
        });

    diagnosticLog("ui-platform", "createContentView complete");
    return page;
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
