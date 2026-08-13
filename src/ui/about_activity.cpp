#ifdef __SWITCH__
#include "about_activity.h"
#include "ui_style.h"
#include "../diagnostics.h"
#include <string>

namespace lunar::ui {

brls::View* AboutActivity::createContentView() {
    const auto& p = uiPalette();

    auto* scroll = new brls::ScrollingFrame();
    scroll->setBackgroundColor(p.background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [](brls::View*) -> bool {
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            return true;
        });

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(26, 48, 36, 48);
    root->setBackgroundColor(p.background);

    auto* header = new brls::Box(brls::Axis::ROW);
    header->setHeight(84);
    header->setAlignItems(brls::AlignItems::CENTER);
    auto* brand = new brls::Box(brls::Axis::COLUMN);
    brand->setGrow(1.0f);
    auto* wordmark = new brls::Label();
    wordmark->setText("LUNARNX");
    wordmark->setFontSize(29);
    wordmark->setTextColor(p.accent);
    brand->addView(wordmark);
    brand->addView(makeMutedLabel(brls::getStr("lunarnx/about/subtitle"), 13));
    header->addView(brand);

    auto* version_chip = new brls::Box(brls::Axis::ROW);
    version_chip->setHeight(52);
    version_chip->setPadding(8, 16, 8, 16);
    version_chip->setBackgroundColor(p.surface_alt);
    version_chip->setBorderThickness(1);
    version_chip->setBorderColor(p.border);
    version_chip->setCornerRadius(4);
    version_chip->setAlignItems(brls::AlignItems::CENTER);
    auto* version_mark = new brls::Label();
    version_mark->setText(brls::getStr("lunarnx/about/version_label"));
    version_mark->setFontSize(11);
    version_mark->setTextColor(p.accent);
    version_chip->addView(version_mark);
    auto* version = new brls::Label();
    version->setText("v" LUNARNX_VERSION);
    version->setFontSize(16);
    version->setTextColor(p.text);
    version->setMarginLeft(10);
    version_chip->addView(version);
    header->addView(version_chip);
    root->addView(header);

    auto* intro = new brls::Box(brls::Axis::ROW);
    intro->setHeight(132);
    intro->setPadding(18, 22, 18, 22);
    intro->setAlignItems(brls::AlignItems::CENTER);
    intro->setMarginBottom(18);
    intro->setBackgroundColor(p.surface_alt);
    intro->setBorderThickness(1);
    intro->setBorderColor(p.border);
    intro->setCornerRadius(0);
    auto* logo = new brls::Label();
    logo->setWidth(82);
    logo->setHeight(82);
    logo->setText("LN");
    logo->setFontSize(24);
    logo->setTextColor(p.accent);
    logo->setBackgroundColor(p.accent_soft);
    logo->setCornerRadius(4);
    logo->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    logo->setVerticalAlign(brls::VerticalAlign::CENTER);
    intro->addView(logo);
    auto* intro_copy = new brls::Box(brls::Axis::COLUMN);
    intro_copy->setGrow(1.0f);
    intro_copy->setPadding(8, 12, 8, 22);
    auto* intro_title = new brls::Label();
    intro_title->setText(brls::getStr("lunarnx/about/title"));
    intro_title->setFontSize(25);
    intro_title->setTextColor(p.text);
    intro_copy->addView(intro_title);
    auto* intro_desc = makeMutedLabel(brls::getStr("lunarnx/about/description"), 14);
    intro_desc->setIsWrapping(true);
    intro_copy->addView(intro_desc);
    intro->addView(intro_copy);
    root->addView(intro);

    auto* community = makeFlatSection(
        brls::getStr("lunarnx/about/community_title"),
        brls::getStr("lunarnx/about/community_subtitle"));
    auto* community_row = new brls::Box(brls::Axis::ROW);
    community_row->setPadding(16, 22, 16, 22);
    community_row->setAlignItems(brls::AlignItems::CENTER);
    community->setMarginBottom(18);
    auto* qq_mark = new brls::Label();
    qq_mark->setWidth(64);
    qq_mark->setHeight(64);
    qq_mark->setText("QQ");
    qq_mark->setFontSize(17);
    qq_mark->setTextColor(p.accent);
    qq_mark->setBackgroundColor(p.accent_soft);
    qq_mark->setCornerRadius(4);
    qq_mark->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    qq_mark->setVerticalAlign(brls::VerticalAlign::CENTER);
    community_row->addView(qq_mark);
    auto* qq_copy = new brls::Box(brls::Axis::COLUMN);
    qq_copy->setGrow(1.0f);
    qq_copy->setPadding(4, 14, 4, 18);
    auto* qq_title = new brls::Label();
    qq_title->setText(brls::getStr("lunarnx/about/qq_group"));
    qq_title->setFontSize(15);
    qq_title->setTextColor(p.text);
    qq_copy->addView(qq_title);
    qq_copy->addView(makeMutedLabel(brls::getStr("lunarnx/about/qq_hint"), 13));
    community_row->addView(qq_copy);
    auto* qq_number = new brls::Label();
    qq_number->setText("736743823");
    qq_number->setFontSize(27);
    qq_number->setTextColor(p.accent);
    qq_number->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    community_row->addView(qq_number);
    addFlatRow(community, community_row);
    root->addView(community);

    root->addView(makeSectionHeader(
        brls::getStr("lunarnx/about/features_title"),
        brls::getStr("lunarnx/about/features_subtitle")));
    auto* features = new brls::Box(brls::Axis::ROW);
    features->setHeight(126);
    features->setMarginBottom(18);
    auto add_feature = [features, &p](const std::string& eyebrow,
                                      const std::string& title,
                                      const std::string& detail,
                                      bool add_margin) {
        auto* card = new brls::Box(brls::Axis::COLUMN);
        card->setGrow(1.0f);
        card->setHeight(126);
        card->setPadding(16, 18, 16, 18);
        card->setBackgroundColor(p.surface_alt);
        card->setBorderThickness(1);
        card->setBorderColor(p.border);
        card->setCornerRadius(0);
        if (add_margin) card->setMarginLeft(10);
        auto* mark = new brls::Label();
        mark->setText(eyebrow);
        mark->setFontSize(11);
        mark->setTextColor(p.accent);
        card->addView(mark);
        auto* heading = new brls::Label();
        heading->setText(title);
        heading->setFontSize(18);
        heading->setTextColor(p.text);
        card->addView(heading);
        auto* copy = makeMutedLabel(detail, 12);
        copy->setIsWrapping(true);
        card->addView(copy);
        features->addView(card);
    };
    add_feature("XBOX", brls::getStr("lunarnx/about/xbox_title"),
                brls::getStr("lunarnx/about/xbox_desc"), false);
    add_feature("XCLOUD", brls::getStr("lunarnx/about/cloud_title"),
                brls::getStr("lunarnx/about/cloud_desc"), true);
    add_feature("PS", brls::getStr("lunarnx/about/ps_title"),
                brls::getStr("lunarnx/about/ps_desc"), true);
    root->addView(features);

    auto* project_section = makeFlatSection(
        brls::getStr("lunarnx/about/open_source_title"));
    auto* project = new brls::Box(brls::Axis::ROW);
    project->setPadding(16, 22, 16, 22);
    project->setAlignItems(brls::AlignItems::CENTER);
    auto* project_copy = new brls::Box(brls::Axis::COLUMN);
    project_copy->setGrow(1.0f);
    auto* project_title = new brls::Label();
    project_title->setText(brls::getStr("lunarnx/about/open_source_desc"));
    project_title->setFontSize(15);
    project_title->setTextColor(p.text);
    project_copy->addView(project_title);
    project_copy->addView(makeMutedLabel(
        brls::getStr("lunarnx/about/components"), 12));
    project->addView(project_copy);
    auto* build = new brls::Label();
    build->setText(
        std::string("AGPL-3.0-only-OpenSSL\n") +
        __DATE__ + "  " + __TIME__);
    build->setFontSize(12);
    build->setTextColor(p.text_muted);
    build->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    project->addView(build);
    addFlatRow(project_section, project);
    root->addView(project_section);

    auto* footer = makeMutedLabel(brls::getStr("lunarnx/about/footer"), 12);
    footer->setHeight(54);
    footer->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    footer->setVerticalAlign(brls::VerticalAlign::CENTER);
    root->addView(footer);

    scroll->setContentView(root);
    return makeAppFrame(brls::getStr("lunarnx/common/about"), scroll);
}

} // namespace lunar::ui
#endif
