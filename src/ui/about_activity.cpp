#ifdef __SWITCH__
#include "about_activity.h"
#include "ui_style.h"
#include "../diagnostics.h"
#include <string>

namespace lunar::ui {

brls::View* AboutActivity::createContentView() {
    const auto& p = uiPalette();

    auto* page = new brls::Box(brls::Axis::COLUMN);
    page->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
    page->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
    page->setBackgroundColor(p.background);
    page->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [](brls::View*) -> bool {
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        });

    auto* header = new brls::Box(brls::Axis::ROW);
    header->setHeight(70);
    header->setPadding(0, 48, 0, 48);
    header->setAlignItems(brls::AlignItems::CENTER);
    auto* wordmark = new brls::Label();
    wordmark->setText("LUNARNX");
    wordmark->setFontSize(28);
    wordmark->setTextColor(p.text);
    wordmark->setGrow(1.0f);
    header->addView(wordmark);
    auto* version = new brls::Label();
    version->setText("v" LUNARNX_VERSION);
    version->setFontSize(13);
    version->setTextColor(p.text_muted);
    version->setBackgroundColor(p.card_muted);
    version->setCornerRadius(8);
    version->setWidth(84);
    version->setHeight(36);
    version->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    version->setVerticalAlign(brls::VerticalAlign::CENTER);
    header->addView(version);
    page->addView(header);

    auto* separator = new brls::Box();
    separator->setHeight(1);
    separator->setBackgroundColor(p.border);
    page->addView(separator);

    auto* content = new brls::Box(brls::Axis::ROW);
    content->setGrow(1.0f);
    content->setPadding(24, 48, 20, 48);

    auto* left = new brls::Box(brls::Axis::COLUMN);
    left->setGrow(1.0f);

    auto* intro = makeUiCard(brls::Axis::ROW);
    intro->setHeight(180);
    intro->setPadding(24, 28, 24, 28);
    intro->setAlignItems(brls::AlignItems::CENTER);
    intro->setBackgroundColor(p.accent_soft);
    intro->setBorderColor(p.accent);
    auto* logo = new brls::Label();
    logo->setWidth(116);
    logo->setHeight(116);
    logo->setText("LN");
    logo->setFontSize(28);
    logo->setTextColor(p.accent);
    logo->setBackgroundColor(p.surface);
    logo->setCornerRadius(8);
    logo->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    logo->setVerticalAlign(brls::VerticalAlign::CENTER);
    intro->addView(logo);
    auto* intro_copy = new brls::Box(brls::Axis::COLUMN);
    intro_copy->setGrow(1.0f);
    intro_copy->setPadding(8, 12, 8, 22);
    auto* intro_title = new brls::Label();
    intro_title->setText(brls::getStr("lunarnx/about/title"));
    intro_title->setFontSize(28);
    intro_title->setTextColor(p.accent);
    intro_copy->addView(intro_title);
    auto* intro_desc = makeMutedLabel(brls::getStr("lunarnx/about/description"), 14);
    intro_desc->setIsWrapping(true);
    intro_copy->addView(intro_desc);
    intro->addView(intro_copy);
    left->addView(intro);

    auto* features_header = makeMutedLabel(
        brls::getStr("lunarnx/about/features_title"), 13);
    features_header->setHeight(42);
    features_header->setVerticalAlign(brls::VerticalAlign::CENTER);
    left->addView(features_header);
    auto* features = new brls::Box(brls::Axis::ROW);
    features->setHeight(280);
    auto add_feature = [features, &p](const std::string& eyebrow,
                                      const std::string& title,
                                      const std::string& detail,
                                      bool add_margin) {
        auto* card = makeUiCard(brls::Axis::COLUMN);
        card->setGrow(1.0f);
        card->setHeight(280);
        card->setPadding(26, 18, 22, 18);
        card->setJustifyContent(brls::JustifyContent::CENTER);
        card->setAlignItems(brls::AlignItems::CENTER);
        if (add_margin) card->setMarginLeft(16);
        auto* mark = new brls::Label();
        mark->setText(eyebrow);
        mark->setWidth(70);
        mark->setHeight(70);
        mark->setFontSize(13);
        mark->setTextColor(p.accent);
        mark->setBackgroundColor(p.surface);
        mark->setCornerRadius(35);
        mark->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        mark->setVerticalAlign(brls::VerticalAlign::CENTER);
        card->addView(mark);
        auto* heading = new brls::Label();
        heading->setText(title);
        heading->setFontSize(16);
        heading->setTextColor(p.text);
        heading->setMarginTop(18);
        heading->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        card->addView(heading);
        auto* copy = makeMutedLabel(detail, 12);
        copy->setIsWrapping(true);
        copy->setMarginTop(8);
        copy->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        card->addView(copy);
        features->addView(card);
    };
    add_feature("XBOX", brls::getStr("lunarnx/about/xbox_title"),
                brls::getStr("lunarnx/about/xbox_desc"), false);
    add_feature("XCLOUD", brls::getStr("lunarnx/about/cloud_title"),
                brls::getStr("lunarnx/about/cloud_desc"), true);
    add_feature("PS", brls::getStr("lunarnx/about/ps_title"),
                brls::getStr("lunarnx/about/ps_desc"), true);
    left->addView(features);
    content->addView(left);

    auto* right = new brls::Box(brls::Axis::COLUMN);
    right->setWidth(310);
    right->setMarginLeft(20);
    auto* community_title = makeMutedLabel(
        brls::getStr("lunarnx/about/community_title"), 13);
    community_title->setHeight(34);
    right->addView(community_title);
    auto* community = makeUiCard(brls::Axis::COLUMN);
    community->setHeight(176);
    community->setAlignItems(brls::AlignItems::CENTER);
    community->setJustifyContent(brls::JustifyContent::CENTER);
    auto* qq = new brls::Label();
    qq->setText("QQ");
    qq->setFontSize(22);
    qq->setTextColor(p.text_muted);
    community->addView(qq);
    auto* qq_number = new brls::Label();
    qq_number->setText("736743823");
    qq_number->setFontSize(20);
    qq_number->setTextColor(p.text);
    qq_number->setMarginTop(14);
    community->addView(qq_number);
    right->addView(community);

    auto* source_title = makeMutedLabel(
        brls::getStr("lunarnx/about/open_source_title"), 13);
    source_title->setHeight(42);
    source_title->setVerticalAlign(brls::VerticalAlign::CENTER);
    right->addView(source_title);
    auto* project = makeUiCard(brls::Axis::COLUMN);
    project->setGrow(1.0f);
    project->setAlignItems(brls::AlignItems::CENTER);
    project->setJustifyContent(brls::JustifyContent::CENTER);
    auto* source_mark = new brls::Label();
    source_mark->setText("< >");
    source_mark->setFontSize(30);
    source_mark->setTextColor(p.text_muted);
    project->addView(source_mark);
    auto* build = new brls::Label();
    build->setText("AGPL-3.0-only-OpenSSL");
    build->setFontSize(15);
    build->setTextColor(p.text);
    build->setMarginTop(18);
    project->addView(build);
    right->addView(project);
    content->addView(right);
    page->addView(content);

    auto* footer = makeHintBar("", brls::getStr("lunarnx/common/back"));
    footer->setWidthPercentage(100.0f);
    footer->setBackgroundColor(p.surface);
    page->addView(footer);
    return page;
}

} // namespace lunar::ui
#endif
