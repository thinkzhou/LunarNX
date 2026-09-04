#ifdef __SWITCH__
#include "about_activity.h"

#include "qr_code.h"
#include "qr_code_view.h"
#include "ui_style.h"

#include <cstdio>
#include <string>

#ifndef LUNARNX_GIT_COMMIT
#define LUNARNX_GIT_COMMIT "unknown"
#endif

namespace lunar::ui {
namespace {

constexpr const char* kRepositoryUrl = "https://github.com/thinkzhou/LunarNX";
constexpr const char* kReleasesUrl =
    "https://github.com/thinkzhou/LunarNX/releases";
constexpr const char* kDiscordInviteUrl = "https://discord.gg/cFZj8mpg2K";
constexpr const char* kDiscordInviteDisplay = "discord.gg/cFZj8mpg2K";

brls::Label* makeLabel(const std::string& text, float size, NVGcolor color,
                       bool wrapping = false) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(size);
    label->setTextColor(color);
    label->setIsWrapping(wrapping);
    return label;
}

brls::Box* makeBadge(const std::string& text) {
    const auto& p = uiPalette();
    auto* badge = new brls::Box(brls::Axis::ROW);
    badge->setHeight(26);
    badge->setPadding(3, 10, 3, 10);
    badge->setBackgroundColor(p.accent_soft);
    badge->setCornerRadius(7);
    auto* label = makeLabel(text, 11, p.accent);
    label->setHeight(20);
    badge->addView(label);
    return badge;
}

brls::Box* makeProjectLink(const std::string& eyebrow,
                           const std::string& url, bool add_margin) {
    const auto& p = uiPalette();
    auto* card = new brls::Box(brls::Axis::COLUMN);
    card->setGrow(1.0f);
    card->setHeight(48);
    card->setPadding(5, 12, 5, 12);
    card->setBackgroundColor(p.surface_alt);
    card->setCornerRadius(8);
    if (add_margin) card->setMarginLeft(8);

    auto* label = makeLabel(eyebrow, 11, p.accent);
    label->setHeight(17);
    label->setSingleLine(true);
    card->addView(label);

    auto* value = makeLabel(url, 11, p.text);
    value->setHeight(19);
    value->setSingleLine(true);
    card->addView(value);
    return card;
}

brls::Box* makeDependencyRow(const std::string& name,
                             const std::string& url) {
    const auto& p = uiPalette();
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setHeight(21);
    row->setAlignItems(brls::AlignItems::CENTER);

    auto* dot = new brls::Box();
    dot->setWidth(4);
    dot->setHeight(4);
    dot->setBackgroundColor(p.accent);
    dot->setCornerRadius(2);
    row->addView(dot);

    auto* name_label = makeLabel(name, 11, p.text);
    name_label->setWidth(146);
    name_label->setHeight(18);
    name_label->setMarginLeft(10);
    name_label->setSingleLine(true);
    row->addView(name_label);
    auto* url_label = makeLabel(url, 10, p.text_muted);
    url_label->setGrow(1.0f);
    url_label->setHeight(17);
    url_label->setSingleLine(true);
    row->addView(url_label);
    return row;
}

brls::Button* makeReleaseCard(const std::string& title,
                              const std::string& date,
                              const std::string& notes) {
    const auto& p = uiPalette();
    auto* card = new brls::Button();
    card->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    card->setHeight(150);
    card->setPadding(0, 0, 0, 0);
    card->setAxis(brls::Axis::ROW);
    card->setBackgroundColor(p.card);
    card->setBorderThickness(1);
    card->setBorderColor(p.border);
    card->setCornerRadius(10);
    card->setHighlightCornerRadius(12);

    auto* accent = new brls::Box();
    accent->setWidth(5);
    accent->setHeight(150);
    accent->setBackgroundColor(p.accent);
    card->addView(accent);

    auto* copy = new brls::Box(brls::Axis::COLUMN);
    copy->setGrow(1.0f);
    copy->setPadding(14, 20, 14, 20);

    auto* header = new brls::Box(brls::Axis::ROW);
    header->setHeight(28);
    header->setAlignItems(brls::AlignItems::CENTER);
    auto* title_label = makeLabel(title, 18, p.text);
    title_label->setGrow(1.0f);
    title_label->setHeight(24);
    header->addView(title_label);
    auto* date_label = makeLabel(date, 12, p.text_muted);
    date_label->setHeight(20);
    header->addView(date_label);
    copy->addView(header);

    auto* body = makeLabel(notes, 13, p.text_muted, true);
    body->setHeight(90);
    body->setMarginTop(6);
    copy->addView(body);
    card->addView(copy);
    card->registerClickAction([title, notes](brls::View*) -> bool {
        auto* dialog = new brls::Dialog(title + "\n\n" + notes);
        dialog->addButton(brls::getStr("lunarnx/common/done"), []() {});
        dialog->open();
        return true;
    });
    return card;
}

bool resourceExists(const std::string& resource) {
    const std::string path = "romfs:/" + resource;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}

brls::Box* makePaymentCard(const std::string& title,
                           const std::string& resource,
                           const std::string& placeholder_mark,
                           bool add_margin) {
    const auto& p = uiPalette();
    auto* card = makeUiCard(brls::Axis::COLUMN);
    card->setGrow(1.0f);
    card->setHeight(392);
    card->setPadding(16, 18, 16, 18);
    card->setAlignItems(brls::AlignItems::CENTER);
    if (add_margin) card->setMarginLeft(14);

    auto* title_label = makeLabel(title, 18, p.text);
    title_label->setHeight(26);
    card->addView(title_label);
    auto* hint = makeMutedLabel(
        brls::getStr("lunarnx/about/scan_to_support"), 11);
    hint->setHeight(18);
    hint->setMarginTop(2);
    card->addView(hint);

    if (resourceExists(resource)) {
        auto* image = new brls::Image();
        image->setWidth(258);
        image->setHeight(258);
        image->setMarginTop(10);
        image->setScalingType(brls::ImageScalingType::FIT);
        image->setInterpolation(brls::ImageInterpolation::NEAREST);
        image->setCornerRadius(8);
        image->setImageFromRes(resource);
        card->addView(image);
    } else {
        auto* placeholder = new brls::Box(brls::Axis::COLUMN);
        placeholder->setWidth(258);
        placeholder->setHeight(258);
        placeholder->setMarginTop(10);
        placeholder->setAlignItems(brls::AlignItems::CENTER);
        placeholder->setJustifyContent(brls::JustifyContent::CENTER);
        placeholder->setBackgroundColor(p.surface_alt);
        placeholder->setCornerRadius(12);
        auto* mark = makeLabel(placeholder_mark, 42, p.accent);
        mark->setHeight(58);
        mark->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        placeholder->addView(mark);
        auto* missing = makeMutedLabel(
            brls::getStr("lunarnx/about/payment_qr_missing"), 12);
        missing->setHeight(22);
        missing->setMarginTop(8);
        missing->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        placeholder->addView(missing);
        card->addView(placeholder);
    }
    return card;
}

brls::Box* makeCommunityCard(const std::string& title,
                             const std::string& detail,
                             const std::string& badge_text,
                             const std::string& resource,
                             const std::string& qr_payload,
                             const std::string& placeholder_mark,
                             bool add_margin) {
    constexpr float kQrSize = 272.0f;
    const auto& p = uiPalette();
    auto* card = makeUiCard(brls::Axis::ROW);
    card->setGrow(1.0f);
    card->setHeight(342);
    card->setPadding(22, 24, 22, 24);
    card->setAlignItems(brls::AlignItems::CENTER);
    if (add_margin) card->setMarginLeft(14);

    if (!qr_payload.empty()) {
        auto* qr = new QrCodeView(kQrSize);
        qr->setQrCode(makeQrCode(qr_payload));
        card->addView(qr);
    } else if (resourceExists(resource)) {
        auto* image = new brls::Image();
        image->setWidth(kQrSize);
        image->setHeight(kQrSize);
        image->setScalingType(brls::ImageScalingType::FIT);
        image->setInterpolation(brls::ImageInterpolation::NEAREST);
        image->setCornerRadius(10);
        image->setImageFromRes(resource);
        card->addView(image);
    } else {
        auto* placeholder = new brls::Box(brls::Axis::COLUMN);
        placeholder->setWidth(kQrSize);
        placeholder->setHeight(kQrSize);
        placeholder->setAlignItems(brls::AlignItems::CENTER);
        placeholder->setJustifyContent(brls::JustifyContent::CENTER);
        placeholder->setBackgroundColor(p.surface_alt);
        placeholder->setCornerRadius(12);
        auto* mark = makeLabel(placeholder_mark, 34, p.accent);
        mark->setWidth(214);
        mark->setHeight(50);
        mark->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        placeholder->addView(mark);
        auto* missing = makeMutedLabel(
            brls::getStr("lunarnx/about/community_qr_missing"), 11);
        missing->setWidth(214);
        missing->setHeight(22);
        missing->setMarginTop(8);
        missing->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        placeholder->addView(missing);
        card->addView(placeholder);
    }

    auto* copy = new brls::Box(brls::Axis::COLUMN);
    copy->setGrow(1.0f);
    copy->setMarginLeft(24);
    copy->setJustifyContent(brls::JustifyContent::CENTER);

    auto* title_label = makeLabel(title, 20, p.text);
    title_label->setHeight(30);
    copy->addView(title_label);

    auto* detail_label = makeLabel(detail, 12, p.text_muted, true);
    detail_label->setHeight(76);
    detail_label->setMarginTop(8);
    copy->addView(detail_label);

    auto* badge = makeBadge(badge_text);
    badge->setWidth(210);
    badge->setJustifyContent(brls::JustifyContent::CENTER);
    badge->setMarginTop(14);
    copy->addView(badge);
    card->addView(copy);
    return card;
}

} // namespace

brls::View* AboutActivity::createContentView() {
    const auto& p = uiPalette();
    auto* workspace = new brls::Box(brls::Axis::COLUMN);
    workspace->setPadding(10, 48, 14, 48);
    workspace->setBackgroundColor(p.background);
    workspace->registerAction(brls::getStr("lunarnx/about/previous_tab"),
        brls::ControllerButton::BUTTON_LB,
        [this](brls::View*) -> bool {
            const int current = static_cast<int>(selected_tab_);
            selectTab(static_cast<Tab>((current + 3) % 4), true);
            return true;
        });
    workspace->registerAction(brls::getStr("lunarnx/about/next_tab"),
        brls::ControllerButton::BUTTON_RB,
        [this](brls::View*) -> bool {
            const int current = static_cast<int>(selected_tab_);
            selectTab(static_cast<Tab>((current + 1) % 4), true);
            return true;
        });

    auto* identity = new brls::Box(brls::Axis::ROW);
    identity->setHeight(64);
    identity->setAlignItems(brls::AlignItems::CENTER);
    auto* logo = makeLabel("LN", 16, p.accent);
    logo->setWidth(44);
    logo->setHeight(44);
    logo->setBackgroundColor(p.accent_soft);
    logo->setCornerRadius(11);
    logo->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    logo->setVerticalAlign(brls::VerticalAlign::CENTER);
    identity->addView(logo);

    auto* identity_copy = new brls::Box(brls::Axis::COLUMN);
    identity_copy->setGrow(1.0f);
    identity_copy->setPadding(5, 16, 5, 16);
    auto* product = makeLabel("LunarNX", 22, p.text);
    product->setHeight(28);
    identity_copy->addView(product);
    auto* subtitle = makeMutedLabel(
        brls::getStr("lunarnx/about/subtitle"), 11);
    subtitle->setHeight(18);
    identity_copy->addView(subtitle);
    identity->addView(identity_copy);

    auto* version = makeLabel("v" LUNARNX_VERSION, 13, p.text_muted);
    version->setWidth(104);
    version->setHeight(34);
    version->setTextColor(p.accent);
    version->setBackgroundColor(p.accent_soft);
    version->setCornerRadius(17);
    version->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    version->setVerticalAlign(brls::VerticalAlign::CENTER);
    identity->addView(version);
    workspace->addView(identity);

    auto* tabs = new brls::Box(brls::Axis::ROW);
    tabs->setHeight(46);
    tabs->setAlignItems(brls::AlignItems::CENTER);
    tab_project_ = makeTabButton(brls::getStr("lunarnx/about/tab_project"));
    tab_changelog_ = makeTabButton(brls::getStr("lunarnx/about/tab_changelog"));
    tab_community_ = makeTabButton(brls::getStr("lunarnx/about/tab_community"));
    tab_support_ = makeTabButton(brls::getStr("lunarnx/about/tab_support"));
    tab_project_->registerClickAction([this](brls::View*) -> bool {
        selectTab(Tab::Project, false);
        return true;
    });
    tab_changelog_->registerClickAction([this](brls::View*) -> bool {
        selectTab(Tab::Changelog, false);
        return true;
    });
    tab_community_->registerClickAction([this](brls::View*) -> bool {
        selectTab(Tab::Community, false);
        return true;
    });
    tab_support_->registerClickAction([this](brls::View*) -> bool {
        selectTab(Tab::Support, false);
        return true;
    });
    tabs->addView(tab_project_);
    tab_changelog_->setMarginLeft(8);
    tabs->addView(tab_changelog_);
    tab_community_->setMarginLeft(8);
    tabs->addView(tab_community_);
    tab_support_->setMarginLeft(8);
    tabs->addView(tab_support_);
    workspace->addView(tabs);

    content_ = new brls::Box(brls::Axis::COLUMN);
    content_->setGrow(1.0f);
    workspace->addView(content_);
    selectTab(Tab::Project, false);

    return makeAppFrame(brls::getStr("lunarnx/common/about"), workspace);
}

brls::Button* AboutActivity::makeTabButton(const std::string& label) {
    auto* button = new brls::Button();
    button->setText(label);
    button->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    button->setGrow(1.0f);
    button->setHeight(38);
    button->setCornerRadius(8);
    button->setHighlightCornerRadius(10);
    button->setFontSize(14);
    return button;
}

void AboutActivity::selectTab(Tab tab, bool focus_tab) {
    if (!content_) return;
    selected_tab_ = tab;
    content_->clearViews();
    switch (tab) {
        case Tab::Project:
            content_->addView(makeProjectTab());
            break;
        case Tab::Changelog:
            content_->addView(makeChangelogTab());
            break;
        case Tab::Community:
            content_->addView(makeCommunityTab());
            break;
        case Tab::Support:
            content_->addView(makeSupportTab());
            break;
    }
    updateTabStyles();
    if (!focus_tab) return;
    brls::Button* target = tab == Tab::Project ? tab_project_
        : tab == Tab::Changelog ? tab_changelog_
        : tab == Tab::Community ? tab_community_ : tab_support_;
    if (target) brls::Application::giveFocus(target);
}

void AboutActivity::updateTabStyles() {
    const auto& p = uiPalette();
    const auto style = [this, &p](brls::Button* button, Tab tab) {
        if (!button) return;
        const bool active = selected_tab_ == tab;
        button->setTextColor(active ? p.accent : p.text_muted);
        button->setBackgroundColor(active ? p.accent_soft
                                          : nvgRGBA(0, 0, 0, 0));
        button->setBorderThickness(active ? 1 : 0);
        button->setBorderColor(active ? p.accent : nvgRGBA(0, 0, 0, 0));
    };
    style(tab_project_, Tab::Project);
    style(tab_changelog_, Tab::Changelog);
    style(tab_community_, Tab::Community);
    style(tab_support_, Tab::Support);
}

brls::View* AboutActivity::makeProjectTab() {
    const auto& p = uiPalette();
    auto* page = new brls::Box(brls::Axis::ROW);
    page->setGrow(1.0f);
    page->setPadding(8, 0, 0, 0);

    auto* author = makeUiCard(brls::Axis::COLUMN);
    author->setWidth(294);
    author->setPadding(16, 18, 16, 18);
    author->setAlignItems(brls::AlignItems::CENTER);

    auto* author_header = new brls::Box(brls::Axis::ROW);
    author_header->setHeight(64);
    author_header->setAlignItems(brls::AlignItems::CENTER);
    auto* avatar = makeLabel("TZ", 21, p.accent);
    avatar->setWidth(58);
    avatar->setHeight(58);
    avatar->setBackgroundColor(p.accent_soft);
    avatar->setCornerRadius(29);
    avatar->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    avatar->setVerticalAlign(brls::VerticalAlign::CENTER);
    author_header->addView(avatar);

    auto* author_copy = new brls::Box(brls::Axis::COLUMN);
    author_copy->setGrow(1.0f);
    author_copy->setPadding(5, 0, 5, 14);
    auto* author_name = makeLabel("thinkzhou", 21, p.text);
    author_name->setHeight(28);
    author_copy->addView(author_name);
    auto* author_role = makeMutedLabel(
        brls::getStr("lunarnx/about/author_role"), 12);
    author_role->setHeight(20);
    author_copy->addView(author_role);
    author_header->addView(author_copy);
    author->addView(author_header);

    auto* repo_qr = new QrCodeView(174);
    repo_qr->setMarginTop(15);
    repo_qr->setQrCode(makeQrCode(kRepositoryUrl));
    author->addView(repo_qr);
    auto* scan = makeMutedLabel(brls::getStr("lunarnx/about/scan_repository"), 11);
    scan->setHeight(34);
    scan->setMarginTop(8);
    scan->setIsWrapping(true);
    scan->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    author->addView(scan);
    page->addView(author);

    auto* project = makeUiCard(brls::Axis::COLUMN);
    project->setGrow(1.0f);
    project->setMarginLeft(16);
    project->setPadding(11, 18, 10, 18);

    auto* overview = new brls::Box(brls::Axis::ROW);
    overview->setHeight(30);
    overview->setAlignItems(brls::AlignItems::CENTER);
    auto* overview_title = makeLabel(
        brls::getStr("lunarnx/about/project_overview"), 20, p.text);
    overview_title->setGrow(1.0f);
    overview_title->setHeight(26);
    overview->addView(overview_title);

    auto* badges = new brls::Box(brls::Axis::ROW);
    badges->setHeight(26);
    badges->addView(makeBadge("Xbox"));
    auto* cloud = makeBadge("xCloud");
    cloud->setMarginLeft(6);
    badges->addView(cloud);
    auto* playstation = makeBadge("PS4 / PS5");
    playstation->setMarginLeft(6);
    badges->addView(playstation);
    overview->addView(badges);
    project->addView(overview);

    auto* description = makeLabel(
        brls::getStr("lunarnx/about/project_description"), 13,
        p.text_muted, true);
    description->setHeight(42);
    description->setMarginTop(4);
    project->addView(description);

    auto* links = new brls::Box(brls::Axis::ROW);
    links->setHeight(48);
    links->setMarginTop(4);
    links->addView(makeProjectLink(
        brls::getStr("lunarnx/about/source_code"), kRepositoryUrl, false));
    links->addView(makeProjectLink(
        brls::getStr("lunarnx/about/releases"), kReleasesUrl, true));
    project->addView(links);

    auto* dependencies_title = makeLabel(
        brls::getStr("lunarnx/about/open_source_projects"), 12, p.text);
    dependencies_title->setHeight(20);
    dependencies_title->setMarginTop(6);
    project->addView(dependencies_title);

    project->addView(makeDependencyRow(
        "XStreaming", "github.com/Geocld/XStreaming"));
    project->addView(makeDependencyRow(
        "PeaSyo", "github.com/Geocld/PeaSyo"));
    project->addView(makeDependencyRow(
        "Moonlight-Switch", "github.com/XITRIX/Moonlight-Switch"));
    project->addView(makeDependencyRow(
        "chiaki-ng", "github.com/chiaki-ng/chiaki-ng"));
    project->addView(makeDependencyRow(
        "Borealis", "github.com/XITRIX/borealis"));
    project->addView(makeDependencyRow(
        "libpeer", "github.com/sepfy/libpeer"));
    project->addView(makeDependencyRow(
        "FFmpeg", "github.com/FFmpeg/FFmpeg"));
    project->addView(makeDependencyRow(
        "libnx", "github.com/switchbrew/libnx"));
    project->addView(makeDependencyRow(
        "deko3d", "github.com/devkitPro/deko3d"));
    project->addView(makeDependencyRow(
        "wiliwili", "github.com/xfangfang/wiliwili"));

    auto* build = new brls::Box(brls::Axis::ROW);
    build->setHeight(20);
    build->setMarginTop(3);
    build->setAlignItems(brls::AlignItems::CENTER);
    auto* license = makeMutedLabel("AGPL-3.0-only-OpenSSL", 11);
    license->setGrow(1.0f);
    build->addView(license);
    build->addView(makeMutedLabel(
        std::string("v") + LUNARNX_VERSION + " · " + LUNARNX_GIT_COMMIT,
        11));
    project->addView(build);
    page->addView(project);
    return page;
}

brls::View* AboutActivity::makeChangelogTab() {
    const auto& p = uiPalette();
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setBackgroundColor(p.background);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setScrollingIndicatorVisible(false);

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(8, 0, 14, 0);
    auto* intro = new brls::Box(brls::Axis::ROW);
    intro->setHeight(58);
    intro->setAlignItems(brls::AlignItems::CENTER);
    auto* intro_copy = new brls::Box(brls::Axis::COLUMN);
    intro_copy->setGrow(1.0f);
    auto* intro_title = makeLabel(
        brls::getStr("lunarnx/about/changelog_title"), 20, p.text);
    intro_title->setHeight(28);
    intro_copy->addView(intro_title);
    auto* intro_subtitle = makeMutedLabel(
        brls::getStr("lunarnx/about/changelog_subtitle"), 12);
    intro_subtitle->setHeight(20);
    intro_copy->addView(intro_subtitle);
    intro->addView(intro_copy);
    auto* releases = makeLabel(kReleasesUrl, 11, p.accent);
    releases->setHeight(20);
    releases->setSingleLine(true);
    intro->addView(releases);
    root->addView(intro);

    auto* current = makeReleaseCard(
        brls::getStr("lunarnx/about/release_020_title"),
        brls::getStr("lunarnx/about/release_020_date"),
        brls::getStr("lunarnx/about/release_020_notes"));
    root->addView(current);
    auto* first = makeReleaseCard(
        brls::getStr("lunarnx/about/release_010_title"),
        brls::getStr("lunarnx/about/release_010_date"),
        brls::getStr("lunarnx/about/release_010_notes"));
    first->setMarginTop(10);
    root->addView(first);
    scroll->setContentView(root);
    return scroll;
}

brls::View* AboutActivity::makeCommunityTab() {
    const auto& p = uiPalette();
    auto* page = new brls::Box(brls::Axis::COLUMN);
    page->setGrow(1.0f);
    page->setPadding(8, 0, 0, 0);

    auto* intro = new brls::Box(brls::Axis::ROW);
    intro->setHeight(58);
    intro->setAlignItems(brls::AlignItems::CENTER);
    auto* intro_copy = new brls::Box(brls::Axis::COLUMN);
    intro_copy->setGrow(1.0f);
    auto* title = makeLabel(
        brls::getStr("lunarnx/about/community_title"), 20, p.text);
    title->setHeight(28);
    intro_copy->addView(title);
    auto* subtitle = makeMutedLabel(
        brls::getStr("lunarnx/about/community_subtitle"), 12);
    subtitle->setHeight(20);
    intro_copy->addView(subtitle);
    intro->addView(intro_copy);

    auto* privacy = makeLabel(
        brls::getStr("lunarnx/about/community_privacy"), 10,
        p.text_muted, true);
    privacy->setWidth(430);
    privacy->setHeight(38);
    privacy->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    intro->addView(privacy);
    page->addView(intro);

    auto* groups = new brls::Box(brls::Axis::ROW);
    groups->setGrow(1.0f);
    groups->addView(makeCommunityCard(
        brls::getStr("lunarnx/about/qq_group"),
        brls::getStr("lunarnx/about/qq_hint"),
        "QQ 736743823", "img/community/qq.png", "", "QQ", false));
    groups->addView(makeCommunityCard(
        brls::getStr("lunarnx/about/international_group"),
        brls::getStr("lunarnx/about/international_hint"),
        kDiscordInviteDisplay, "", kDiscordInviteUrl, "DISCORD", true));
    page->addView(groups);
    return page;
}

brls::View* AboutActivity::makeSupportTab() {
    const auto& p = uiPalette();
    auto* page = new brls::Box(brls::Axis::ROW);
    page->setGrow(1.0f);
    page->setPadding(8, 0, 0, 0);

    auto* message = makeUiCard(brls::Axis::COLUMN);
    message->setWidth(306);
    message->setHeight(392);
    message->setPadding(22, 22, 22, 22);
    message->setAlignItems(brls::AlignItems::CENTER);
    auto* heart_badge = new brls::Box(brls::Axis::COLUMN);
    heart_badge->setWidth(76);
    heart_badge->setHeight(76);
    heart_badge->setBackgroundColor(p.accent_soft);
    heart_badge->setCornerRadius(38);
    heart_badge->setAlignItems(brls::AlignItems::CENTER);
    heart_badge->setJustifyContent(brls::JustifyContent::CENTER);
    auto* heart = makeLabel("♥", 38, p.accent);
    heart->setWidth(76);
    heart->setHeight(58);
    heart->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    heart->setVerticalAlign(brls::VerticalAlign::CENTER);
    heart_badge->addView(heart);
    message->addView(heart_badge);
    auto* thanks = makeLabel(
        brls::getStr("lunarnx/about/support_thanks"), 21, p.text);
    thanks->setHeight(32);
    thanks->setMarginTop(14);
    thanks->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    message->addView(thanks);
    auto* support_copy = makeLabel(
        brls::getStr("lunarnx/about/support_message"), 13,
        p.text_muted, true);
    support_copy->setHeight(128);
    support_copy->setMarginTop(12);
    support_copy->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    message->addView(support_copy);
    auto* author = makeBadge("thinkzhou");
    author->setMarginTop(14);
    message->addView(author);
    page->addView(message);

    auto* payment = new brls::Box(brls::Axis::ROW);
    payment->setGrow(1.0f);
    payment->setMarginLeft(16);
    payment->addView(makePaymentCard(
        brls::getStr("lunarnx/about/wechat_pay"),
        "img/support/wechat.png", "微", false));
    payment->addView(makePaymentCard(
        brls::getStr("lunarnx/about/alipay"),
        "img/support/alipay.png", "支", true));
    page->addView(payment);
    return page;
}

} // namespace lunar::ui
#endif
