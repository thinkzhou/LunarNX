#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>

#include <string>

namespace lunar::ui {

struct UiPalette {
    NVGcolor background;
    NVGcolor surface;
    NVGcolor surface_alt;
    NVGcolor card;
    NVGcolor card_muted;
    NVGcolor accent;
    NVGcolor accent_soft;
    NVGcolor text;
    NVGcolor text_muted;
    NVGcolor border;
    NVGcolor xbox_green;
    NVGcolor ps_blue;
    NVGcolor success;
    NVGcolor warning;
    NVGcolor error;
    NVGcolor stream_overlay;
};

void installLunarTheme();
const UiPalette& uiPalette();

brls::Box* makeUiCard(brls::Axis axis = brls::Axis::COLUMN);
brls::Box* makeSectionHeader(const std::string& title,
                             const std::string& subtitle = "");
brls::Label* makeMutedLabel(const std::string& text, float font_size = 14);
void stylePrimaryButton(brls::Button* button);
void styleSecondaryButton(brls::Button* button);
void styleQuietButton(brls::Button* button);

// A/B controller hint bar. Empty strings omit that hint.
brls::Box* makeHintBar(const std::string& a_hint = "",
                       const std::string& b_hint = "");

class ConsoleGlyphView : public brls::View {
public:
    ConsoleGlyphView(std::string console_type, bool online);

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    std::string console_type_;
    bool online_ = false;
};

} // namespace lunar::ui
#endif
