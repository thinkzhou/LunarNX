#ifdef __SWITCH__

#include "applet_mode_activity.h"

#include "ui_style.h"
#include "../diagnostics.h"

#include <string>

namespace lunar::ui {
namespace {

brls::Box* makeLaunchStep(const std::string& number,
                          const std::string& title,
                          const std::string& detail) {
    const auto& palette = uiPalette();
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setWidth(780);
    row->setHeight(66);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setMarginTop(8);

    auto* badge = new brls::Label();
    badge->setWidth(38);
    badge->setHeight(38);
    badge->setText(number);
    badge->setFontSize(15);
    badge->setTextColor(palette.accent);
    badge->setBackgroundColor(palette.accent_soft);
    badge->setCornerRadius(19);
    badge->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    badge->setVerticalAlign(brls::VerticalAlign::CENTER);
    row->addView(badge);

    auto* copy = new brls::Box(brls::Axis::COLUMN);
    copy->setGrow(1.0f);
    copy->setMarginLeft(16);

    auto* heading = new brls::Label();
    heading->setText(title);
    heading->setFontSize(15);
    heading->setTextColor(palette.text);
    copy->addView(heading);

    auto* description = makeMutedLabel(detail, 12);
    description->setIsWrapping(true);
    description->setWidth(720);
    description->setMarginTop(3);
    copy->addView(description);
    row->addView(copy);
    return row;
}

void quitApplication() {
    diagnosticLog("ui-applet-mode", "exit requested");
    brls::Application::quit();
}

} // namespace

brls::View* AppletModeActivity::createContentView() {
    const auto& palette = uiPalette();
    diagnosticLog("ui-applet-mode", "showing full application mode instructions");

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setBackgroundColor(palette.background);
    root->setAlignItems(brls::AlignItems::CENTER);
    root->setJustifyContent(brls::JustifyContent::CENTER);
    root->setPadding(18, 32, 24, 32);
    root->registerAction(brls::getStr("lunarnx/common/exit"),
        brls::ControllerButton::BUTTON_B,
        [](brls::View*) -> bool {
            quitApplication();
            return true;
        });

    auto* card = makeUiCard(brls::Axis::COLUMN);
    card->setWidth(880);
    card->setPadding(24, 48, 24, 48);
    card->setAlignItems(brls::AlignItems::CENTER);

    auto* eyebrow = new brls::Label();
    eyebrow->setText(brls::getStr("lunarnx/launch_mode/eyebrow"));
    eyebrow->setFontSize(12);
    eyebrow->setTextColor(palette.warning);
    eyebrow->setMarginBottom(8);
    card->addView(eyebrow);

    auto* title = new brls::Label();
    title->setText(brls::getStr("lunarnx/launch_mode/title"));
    title->setFontSize(23);
    title->setTextColor(palette.text);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setMarginBottom(8);
    card->addView(title);

    auto* description = makeMutedLabel(
        brls::getStr("lunarnx/launch_mode/description"), 13);
    description->setWidth(780);
    description->setIsWrapping(true);
    description->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    description->setMarginBottom(6);
    card->addView(description);

    card->addView(makeLaunchStep(
        "1", brls::getStr("lunarnx/launch_mode/step1_title"),
        brls::getStr("lunarnx/launch_mode/step1_detail")));
    card->addView(makeLaunchStep(
        "2", brls::getStr("lunarnx/launch_mode/step2_title"),
        brls::getStr("lunarnx/launch_mode/step2_detail")));
    card->addView(makeLaunchStep(
        "3", brls::getStr("lunarnx/launch_mode/step3_title"),
        brls::getStr("lunarnx/launch_mode/step3_detail")));

    auto* note = makeMutedLabel(
        brls::getStr("lunarnx/launch_mode/note"), 11);
    note->setWidth(780);
    note->setIsWrapping(true);
    note->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    note->setMarginTop(10);
    card->addView(note);

    auto* exit = new brls::Button();
    exit->setWidth(260);
    exit->setText(brls::getStr("lunarnx/launch_mode/exit"));
    exit->setMarginTop(18);
    stylePrimaryButton(exit);
    exit->registerClickAction([](brls::View*) -> bool {
        quitApplication();
        return true;
    });
    card->addView(exit);

    root->addView(card);
    return makeAppFrame("LunarNX", root);
}

} // namespace lunar::ui

#endif
