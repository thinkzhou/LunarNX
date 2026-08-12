#ifdef __SWITCH__
#include "ui_style.h"

#include <algorithm>
#include <utility>

namespace lunar::ui {
namespace {

const UiPalette kDarkPalette{
    .background = nvgRGB(10, 15, 12),
    .surface = nvgRGB(15, 23, 18),
    .surface_alt = nvgRGB(20, 31, 24),
    .card = nvgRGB(18, 28, 22),
    .card_muted = nvgRGB(24, 35, 29),
    .accent = nvgRGB(105, 216, 92),
    .accent_soft = nvgRGBA(75, 182, 66, 72),
    .text = nvgRGB(244, 248, 243),
    .text_muted = nvgRGB(159, 178, 164),
    .border = nvgRGB(53, 72, 59),
    .success = nvgRGB(105, 216, 92),
    .warning = nvgRGB(244, 197, 74),
    .error = nvgRGB(255, 124, 115),
    .stream_overlay = nvgRGBA(7, 12, 9, 218),
};

const UiPalette kLightPalette{
    .background = nvgRGB(242, 246, 240),
    .surface = nvgRGB(250, 252, 248),
    .surface_alt = nvgRGB(231, 239, 228),
    .card = nvgRGB(252, 254, 251),
    .card_muted = nvgRGB(235, 242, 232),
    .accent = nvgRGB(16, 124, 16),
    .accent_soft = nvgRGBA(16, 124, 16, 42),
    .text = nvgRGB(24, 31, 25),
    .text_muted = nvgRGB(91, 108, 95),
    .border = nvgRGB(190, 204, 191),
    .success = nvgRGB(16, 124, 16),
    .warning = nvgRGB(169, 117, 0),
    .error = nvgRGB(186, 43, 37),
    .stream_overlay = nvgRGBA(8, 14, 10, 218),
};

void installThemeVariant(brls::Theme& theme, const UiPalette& p, bool dark) {
    theme.addColor("brls/background", p.background);
    theme.addColor("brls/text", p.text);
    theme.addColor("brls/text_disabled", p.text_muted);
    theme.addColor("brls/accent", p.accent);
    theme.addColor("brls/click_pulse", p.accent_soft);
    theme.addColor("brls/highlight/background", dark
        ? nvgRGB(24, 39, 29)
        : nvgRGB(229, 245, 226));
    theme.addColor("brls/highlight/color1", p.accent);
    theme.addColor("brls/highlight/color2", dark
        ? nvgRGB(171, 246, 158)
        : nvgRGB(74, 170, 68));
    theme.addColor("brls/applet_frame/separator", p.border);
    theme.addColor("brls/sidebar/background", p.surface);
    theme.addColor("brls/sidebar/active_item", p.accent);
    theme.addColor("brls/sidebar/separator", p.border);
    theme.addColor("brls/header/border", p.border);
    theme.addColor("brls/header/rectangle", p.accent);
    theme.addColor("brls/header/subtitle", p.text_muted);
    theme.addColor("brls/button/primary_enabled_background", dark
        ? nvgRGB(35, 132, 38)
        : nvgRGB(16, 124, 16));
    theme.addColor("brls/button/primary_disabled_background", p.card_muted);
    theme.addColor("brls/button/primary_enabled_text", nvgRGB(255, 255, 255));
    theme.addColor("brls/button/primary_disabled_text", p.text_muted);
    theme.addColor("brls/button/default_enabled_background", p.card_muted);
    theme.addColor("brls/button/default_disabled_background", p.surface_alt);
    theme.addColor("brls/button/default_enabled_text", p.text);
    theme.addColor("brls/button/default_disabled_text", p.text_muted);
    theme.addColor("brls/button/highlight_enabled_text", p.accent);
    theme.addColor("brls/button/highlight_disabled_text", p.text_muted);
    theme.addColor("brls/button/enabled_border_color", p.border);
    theme.addColor("brls/button/disabled_border_color", p.border);
    theme.addColor("brls/list/listItem_value_color", p.accent);
    theme.addColor("brls/slider/line_filled", p.accent);
    theme.addColor("brls/slider/line_empty", p.border);
    theme.addColor("brls/spinner/bar_color", nvgRGBA(
        static_cast<unsigned char>(p.accent.r * 255.0f),
        static_cast<unsigned char>(p.accent.g * 255.0f),
        static_cast<unsigned char>(p.accent.b * 255.0f),
        150));
}

} // namespace

void installLunarTheme() {
    installThemeVariant(brls::Theme::getDarkTheme(), kDarkPalette, true);
    installThemeVariant(brls::Theme::getLightTheme(), kLightPalette, false);
}

const UiPalette& uiPalette() {
    return brls::Application::getThemeVariant() == brls::ThemeVariant::DARK
        ? kDarkPalette
        : kLightPalette;
}

brls::Box* makeUiCard(brls::Axis axis) {
    const auto& p = uiPalette();
    auto* card = new brls::Box(axis);
    card->setBackgroundColor(p.card);
    card->setBorderThickness(1);
    card->setBorderColor(p.border);
    card->setCornerRadius(16);
    card->setPadding(18, 20, 18, 20);
    return card;
}

brls::Box* makeSectionHeader(const std::string& title,
                             const std::string& subtitle) {
    const auto& p = uiPalette();
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setHeight(subtitle.empty() ? 48 : 58);
    row->setAlignItems(brls::AlignItems::CENTER);

    auto* accent = new brls::Box();
    accent->setWidth(5);
    accent->setHeight(subtitle.empty() ? 24 : 34);
    accent->setCornerRadius(3);
    accent->setBackgroundColor(p.accent);
    row->addView(accent);

    auto* text = new brls::Box(brls::Axis::COLUMN);
    text->setGrow(1.0f);
    text->setPadding(0, 0, 0, 14);

    auto* title_label = new brls::Label();
    title_label->setText(title);
    title_label->setFontSize(22);
    title_label->setTextColor(p.text);
    text->addView(title_label);

    if (!subtitle.empty()) {
        auto* subtitle_label = new brls::Label();
        subtitle_label->setText(subtitle);
        subtitle_label->setFontSize(13);
        subtitle_label->setTextColor(p.text_muted);
        text->addView(subtitle_label);
    }
    row->addView(text);
    return row;
}

brls::Label* makeMutedLabel(const std::string& text, float font_size) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(font_size);
    label->setTextColor(uiPalette().text_muted);
    return label;
}

void stylePrimaryButton(brls::Button* button) {
    if (!button) return;
    button->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    button->setHeight(48);
    button->setCornerRadius(12);
    button->setHighlightCornerRadius(14);
}

void styleSecondaryButton(brls::Button* button) {
    if (!button) return;
    button->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    button->setHeight(48);
    button->setCornerRadius(12);
    button->setHighlightCornerRadius(14);
}

void styleQuietButton(brls::Button* button) {
    if (!button) return;
    button->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    button->setHeight(44);
    button->setHighlightCornerRadius(12);
}

brls::AppletFrame* makeAppFrame(const std::string& title,
                                brls::View* content) {
    auto* frame = new brls::AppletFrame(content);
    frame->setTitle(title);
    return frame;
}

brls::Button* makeSidebarButton(const std::string& label, bool active) {
    auto* button = new brls::Button();
    button->setText(label);
    button->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    button->setHeight(58);
    button->setCornerRadius(0);
    button->setHighlightCornerRadius(6);
    button->setJustifyContent(brls::JustifyContent::FLEX_START);
    button->setPaddingLeft(18);
    setSidebarButtonActive(button, active);
    return button;
}

void setSidebarButtonActive(brls::Button* button, bool active) {
    if (!button) return;
    const auto& p = uiPalette();
    button->setTextColor(active ? p.accent : p.text);
    button->setBorderThickness(active ? 3 : 0);
    button->setBorderColor(active ? p.accent : p.surface);
}

brls::Box* makePageHeading(const std::string& title,
                           const std::string& subtitle) {
    const auto& p = uiPalette();
    auto* heading = new brls::Box(brls::Axis::COLUMN);
    heading->setHeight(subtitle.empty() ? 48 : 62);
    auto* title_label = new brls::Label();
    title_label->setText(title);
    title_label->setFontSize(25);
    title_label->setTextColor(p.text);
    heading->addView(title_label);
    if (!subtitle.empty()) {
        heading->addView(makeMutedLabel(subtitle, 13));
    }
    return heading;
}

ConsoleGlyphView::ConsoleGlyphView(std::string console_type, bool online)
    : console_type_(std::move(console_type)), online_(online) {
    setWidth(112);
    setHeight(96);
}

void ConsoleGlyphView::draw(NVGcontext* vg, float x, float y,
                            float width, float height,
                            brls::Style style, brls::FrameContext* ctx) {
    (void)style;
    (void)ctx;
    const auto& p = uiPalette();
    const float alpha = online_ ? 1.0f : 0.52f;
    const bool series_s = console_type_.find("SeriesS") != std::string::npos;
    const bool series_x = console_type_.find("SeriesX") != std::string::npos;
    const bool is_ps4 = console_type_.find("PS4") != std::string::npos;
    const bool is_ps5 = console_type_.find("PS5") != std::string::npos;
    const bool is_ps  = is_ps4 || is_ps5;

    nvgSave(vg);
    nvgGlobalAlpha(vg, alpha);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 6, y + 6, width - 12, height - 12, 18);
    nvgFillColor(vg, p.surface_alt);
    nvgFill(vg);

    if (series_s) {
        const float tower_w = 35;
        const float tower_h = 68;
        const float tx = x + (width - tower_w) * 0.5f;
        const float ty = y + (height - tower_h) * 0.5f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, tx, ty, tower_w, tower_h, 5);
        nvgFillColor(vg, nvgRGB(238, 241, 237));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, tx + tower_w * 0.5f, ty + 18, 11);
        nvgFillColor(vg, nvgRGB(35, 40, 36));
        nvgFill(vg);
    } else if (series_x) {
        const float tower_w = 42;
        const float tower_h = 70;
        const float tx = x + (width - tower_w) * 0.5f;
        const float ty = y + (height - tower_h) * 0.5f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, tx, ty, tower_w, tower_h, 5);
        nvgFillColor(vg, nvgRGB(31, 34, 32));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, tx + 5, ty + 5, tower_w - 10, 8, 4);
        nvgFillColor(vg, p.accent);
        nvgFill(vg);
    } else if (is_ps) {
        // PlayStation glyph: tilted square with PS colors
        const float cx = x + width * 0.5f;
        const float cy = y + height * 0.5f;
        const float ps_w = 52;
        const float ps_h = 52;
        const float ps_x = cx - ps_w * 0.5f;
        const float ps_y = cy - ps_h * 0.5f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, ps_x, ps_y, ps_w, ps_h, 8);
        nvgFillColor(vg, nvgRGB(0, 67, 156));
        nvgFill(vg);

        // "PS" text
        nvgFontSize(vg, 22);
        nvgFontFace(vg, "sans-bold");
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgText(vg, cx, cy, "PS", nullptr);

        if (is_ps5) {
            // Small "5" badge
            nvgBeginPath(vg);
            nvgCircle(vg, ps_x + ps_w - 10, ps_y + 10, 10);
            nvgFillColor(vg, nvgRGB(255, 255, 255));
            nvgFill(vg);
            nvgFontSize(vg, 12);
            nvgFillColor(vg, nvgRGB(0, 67, 156));
            nvgText(vg, ps_x + ps_w - 10, ps_y + 11, "5", nullptr);
        } else {
            // Small "4" badge
            nvgBeginPath(vg);
            nvgCircle(vg, ps_x + ps_w - 10, ps_y + 10, 10);
            nvgFillColor(vg, nvgRGB(255, 255, 255));
            nvgFill(vg);
            nvgFontSize(vg, 12);
            nvgFillColor(vg, nvgRGB(0, 67, 156));
            nvgText(vg, ps_x + ps_w - 10, ps_y + 11, "4", nullptr);
        }
    } else {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 25, y + 24, width - 50, height - 48, 8);
        nvgStrokeWidth(vg, 4);
        nvgStrokeColor(vg, p.text_muted);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, x + width * 0.5f, y + height * 0.5f, 8);
        nvgFillColor(vg, p.accent);
        nvgFill(vg);
    }

    nvgBeginPath(vg);
    nvgCircle(vg, x + width - 20, y + 20, 6);
    nvgFillColor(vg, online_ ? p.success : p.text_muted);
    nvgFill(vg);
    nvgRestore(vg);
}

} // namespace lunar::ui
#endif
