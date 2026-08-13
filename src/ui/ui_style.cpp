#ifdef __SWITCH__
#include "ui_style.h"

#include <algorithm>
#include <utility>

namespace lunar::ui {
namespace {

const UiPalette kDarkPalette{
    .background = nvgRGB(27, 30, 35),
    .surface = nvgRGB(35, 39, 46),
    .surface_alt = nvgRGB(42, 47, 55),
    .card = nvgRGB(38, 43, 50),
    .card_muted = nvgRGB(48, 54, 63),
    .accent = nvgRGB(0, 217, 195),
    .accent_soft = nvgRGBA(0, 217, 195, 40),
    .text = nvgRGB(244, 246, 248),
    .text_muted = nvgRGB(154, 163, 173),
    .border = nvgRGB(58, 65, 75),
    .xbox_green = nvgRGB(16, 124, 16),
    .ps_blue = nvgRGB(0, 112, 209),
    .success = nvgRGB(76, 195, 138),
    .warning = nvgRGB(224, 169, 75),
    .error = nvgRGB(229, 96, 90),
    .stream_overlay = nvgRGBA(8, 11, 14, 218),
};

const UiPalette kLightPalette{
    .background = nvgRGB(241, 243, 245),
    .surface = nvgRGB(248, 249, 250),
    .surface_alt = nvgRGB(232, 236, 240),
    .card = nvgRGB(252, 253, 254),
    .card_muted = nvgRGB(236, 240, 244),
    .accent = nvgRGB(0, 122, 110),
    .accent_soft = nvgRGBA(0, 122, 110, 40),
    .text = nvgRGB(30, 34, 39),
    .text_muted = nvgRGB(105, 113, 122),
    .border = nvgRGB(198, 205, 212),
    .xbox_green = nvgRGB(16, 124, 16),
    .ps_blue = nvgRGB(0, 112, 209),
    .success = nvgRGB(20, 128, 74),
    .warning = nvgRGB(161, 110, 0),
    .error = nvgRGB(186, 43, 37),
    .stream_overlay = nvgRGBA(8, 11, 14, 218),
};

void installThemeVariant(brls::Theme& theme, const UiPalette& p, bool dark) {
    theme.addColor("brls/background", p.background);
    theme.addColor("brls/text", p.text);
    theme.addColor("brls/text_disabled", p.text_muted);
    theme.addColor("brls/accent", p.accent);
    theme.addColor("brls/click_pulse", p.accent_soft);
    theme.addColor("brls/highlight/background", dark
        ? nvgRGB(42, 47, 55)
        : nvgRGB(226, 240, 236));
    theme.addColor("brls/highlight/color1", p.accent);
    theme.addColor("brls/highlight/color2", dark
        ? nvgRGB(108, 255, 240)
        : nvgRGB(0, 160, 145));
    theme.addColor("brls/applet_frame/separator", p.border);
    theme.addColor("brls/sidebar/background", p.surface);
    theme.addColor("brls/sidebar/active_item", p.accent);
    theme.addColor("brls/sidebar/separator", p.border);
    theme.addColor("brls/header/border", p.border);
    theme.addColor("brls/header/rectangle", p.accent);
    theme.addColor("brls/header/subtitle", p.text_muted);
    theme.addColor("brls/button/primary_enabled_background", p.accent);
    theme.addColor("brls/button/primary_disabled_background", p.card_muted);
    theme.addColor("brls/button/primary_enabled_text", nvgRGB(16, 22, 20));
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
    card->setCornerRadius(8);
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
    accent->setWidth(4);
    accent->setHeight(subtitle.empty() ? 24 : 34);
    accent->setCornerRadius(2);
    accent->setBackgroundColor(p.accent);
    row->addView(accent);

    auto* text = new brls::Box(brls::Axis::COLUMN);
    text->setGrow(1.0f);
    text->setPadding(0, 0, 0, 12);

    auto* title_label = new brls::Label();
    title_label->setText(title);
    title_label->setFontSize(21);
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
    button->setCornerRadius(8);
    button->setHighlightCornerRadius(10);
}

void styleSecondaryButton(brls::Button* button) {
    if (!button) return;
    button->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    button->setHeight(48);
    button->setCornerRadius(8);
    button->setHighlightCornerRadius(10);
}

void styleQuietButton(brls::Button* button) {
    if (!button) return;
    button->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    button->setHeight(44);
    button->setHighlightCornerRadius(10);
}

brls::Box* makeHintBar(const std::string& a_hint, const std::string& b_hint) {
    const auto& p = uiPalette();
    auto* bar = new brls::Box(brls::Axis::ROW);
    bar->setHeight(44);
    bar->setAlignItems(brls::AlignItems::CENTER);
    bar->setPadding(24, 0, 24, 0);

    if (!a_hint.empty()) {
        auto* label = new brls::Label();
        label->setText(brls::Hint::getKeyIcon(brls::ControllerButton::BUTTON_A) +
                       "  " + a_hint);
        label->setFontSize(15);
        label->setTextColor(p.text_muted);
        bar->addView(label);
    }

    if (!b_hint.empty()) {
        auto* label = new brls::Label();
        label->setText(brls::Hint::getKeyIcon(brls::ControllerButton::BUTTON_B) +
                       "  " + b_hint);
        label->setFontSize(15);
        label->setTextColor(p.text_muted);
        label->setMarginLeft(a_hint.empty() ? 0 : 28);
        bar->addView(label);
    }

    return bar;
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
    nvgRoundedRect(vg, x + 6, y + 6, width - 12, height - 12, 16);
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
        nvgFillColor(vg, p.xbox_green);
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
        nvgFillColor(vg, p.ps_blue);
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
            nvgFillColor(vg, p.ps_blue);
            nvgText(vg, ps_x + ps_w - 10, ps_y + 11, "5", nullptr);
        } else {
            // Small "4" badge
            nvgBeginPath(vg);
            nvgCircle(vg, ps_x + ps_w - 10, ps_y + 10, 10);
            nvgFillColor(vg, nvgRGB(255, 255, 255));
            nvgFill(vg);
            nvgFontSize(vg, 12);
            nvgFillColor(vg, p.ps_blue);
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
