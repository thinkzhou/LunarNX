#ifdef __SWITCH__
#include "about_activity.h"
#include "ui_style.h"
#include <string>

namespace lunar::ui {
namespace {

brls::Box* makeInfoCell(const std::string& label, const std::string& value,
                        const std::string& detail, bool add_margin) {
    const auto& p = uiPalette();
    auto* cell = new brls::Box(brls::Axis::COLUMN);
    cell->setGrow(1.0f);
    cell->setHeight(96);
    cell->setPadding(14, 18, 14, 18);
    cell->setBackgroundColor(p.surface_alt);
    cell->setBorderThickness(1);
    cell->setBorderColor(p.border);
    cell->setCornerRadius(8);
    if (add_margin) cell->setMarginLeft(12);

    auto* eyebrow = new brls::Label();
    eyebrow->setText(label);
    eyebrow->setFontSize(11);
    eyebrow->setTextColor(p.accent);
    cell->addView(eyebrow);

    auto* title = new brls::Label();
    title->setText(value);
    title->setFontSize(16);
    title->setTextColor(p.text);
    title->setMarginTop(5);
    cell->addView(title);

    auto* copy = makeMutedLabel(detail, 11);
    copy->setIsWrapping(false);
    copy->setMarginTop(3);
    cell->addView(copy);
    return cell;
}

brls::Box* makeMetadataRow(const std::string& label, const std::string& value,
                           bool add_margin) {
    const auto& p = uiPalette();
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setWidth(586);
    row->setHeight(58);
    row->setPadding(0, 16, 0, 16);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setBackgroundColor(p.surface);
    row->setBorderThickness(1);
    row->setBorderColor(p.border);
    row->setCornerRadius(8);
    if (add_margin) row->setMarginLeft(12);

    auto* key = makeMutedLabel(label, 12);
    key->setWidth(116);
    row->addView(key);
    auto* text = new brls::Label();
    text->setGrow(1.0f);
    text->setText(value);
    text->setFontSize(12);
    text->setTextColor(p.text);
    text->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    row->addView(text);
    return row;
}

} // namespace

brls::View* AboutActivity::createContentView() {
    const auto& p = uiPalette();

    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(18, 48, 18, 48);
    root->setBackgroundColor(p.background);
    root->setFocusable(true);
    root->setHideHighlight(true);
    auto* identity = new brls::Box(brls::Axis::ROW);
    identity->setHeight(92);
    identity->setAlignItems(brls::AlignItems::CENTER);
    identity->setMarginBottom(16);

    auto* logo = new brls::Label();
    logo->setWidth(64);
    logo->setHeight(64);
    logo->setText("LN");
    logo->setFontSize(19);
    logo->setTextColor(p.accent);
    logo->setBackgroundColor(p.accent_soft);
    logo->setCornerRadius(8);
    logo->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    logo->setVerticalAlign(brls::VerticalAlign::CENTER);
    identity->addView(logo);

    auto* identity_copy = new brls::Box(brls::Axis::COLUMN);
    identity_copy->setGrow(1.0f);
    identity_copy->setPadding(10, 18, 10, 18);
    auto* product = new brls::Label();
    product->setText("LunarNX");
    product->setFontSize(22);
    product->setTextColor(p.text);
    identity_copy->addView(product);
    auto* description = makeMutedLabel(
        brls::getStr("lunarnx/about/description"), 13);
    description->setIsWrapping(false);
    identity_copy->addView(description);
    identity->addView(identity_copy);

    auto* version = new brls::Label();
    version->setWidth(112);
    version->setHeight(38);
    version->setText("v" LUNARNX_VERSION);
    version->setFontSize(14);
    version->setTextColor(p.text_muted);
    version->setBackgroundColor(p.surface_alt);
    version->setBorderThickness(1);
    version->setBorderColor(p.border);
    version->setCornerRadius(8);
    version->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    version->setVerticalAlign(brls::VerticalAlign::CENTER);
    identity->addView(version);
    root->addView(identity);

    auto* divider = new brls::Box();
    divider->setHeight(1);
    divider->setBackgroundColor(p.border);
    divider->setMarginBottom(16);
    root->addView(divider);

    root->addView(makeSectionHeader(
        brls::getStr("lunarnx/about/features_title")));
    auto* capabilities = new brls::Box(brls::Axis::ROW);
    capabilities->setHeight(96);
    capabilities->setMarginBottom(16);
    capabilities->addView(makeInfoCell(
        "XBOX", brls::getStr("lunarnx/about/xbox_title"),
        brls::getStr("lunarnx/about/xbox_desc"), false));
    capabilities->addView(makeInfoCell(
        "XCLOUD", brls::getStr("lunarnx/about/cloud_title"),
        brls::getStr("lunarnx/about/cloud_desc"), true));
    capabilities->addView(makeInfoCell(
        "PLAYSTATION", brls::getStr("lunarnx/about/ps_title"),
        brls::getStr("lunarnx/about/ps_desc"), true));
    root->addView(capabilities);

    root->addView(makeSectionHeader(
        brls::getStr("lunarnx/about/project_title")));
    auto* first_row = new brls::Box(brls::Axis::ROW);
    first_row->setHeight(58);
    first_row->setMarginBottom(10);
    first_row->addView(makeMetadataRow(
        brls::getStr("lunarnx/about/repository"),
        "github.com/thinkzhou/LunarNX", false));
    first_row->addView(makeMetadataRow(
        brls::getStr("lunarnx/about/qq_group"), "736743823", true));
    root->addView(first_row);

    auto* second_row = new brls::Box(brls::Axis::ROW);
    second_row->setHeight(58);
    second_row->setMarginBottom(14);
    second_row->addView(makeMetadataRow(
        brls::getStr("lunarnx/about/license"),
        "AGPL-3.0-only-OpenSSL", false));
    second_row->addView(makeMetadataRow(
        brls::getStr("lunarnx/about/acknowledgements"),
        brls::getStr("lunarnx/about/components_short"), true));
    root->addView(second_row);

    auto* build = new brls::Box(brls::Axis::ROW);
    build->setHeight(34);
    build->setAlignItems(brls::AlignItems::CENTER);
    auto* build_label = makeMutedLabel(
        brls::getStr("lunarnx/about/build_label"), 11);
    build->addView(build_label);
    auto* build_value = makeMutedLabel(
        std::string(__DATE__) + "  " + __TIME__, 11);
    build_value->setGrow(1.0f);
    build_value->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    build->addView(build_value);
    root->addView(build);

    return makeAppFrame(brls::getStr("lunarnx/common/about"), root);
}

} // namespace lunar::ui
#endif
