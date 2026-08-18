#ifdef __SWITCH__
#include "cloud_game_card.h"

#include "ui_style.h"

#include <utility>

namespace lunar::ui {

CloudGameCard::CloudGameCard(api::CloudTitle title, bool is_new,
                             PosterLoader::BatchId poster_batch,
                             SelectHandler on_select)
    : brls::Box(brls::Axis::COLUMN), title_(std::move(title)) {
    const auto& palette = uiPalette();
    setWidth(218);
    setHeight(370);
    setPadding(8, 8, 8, 8);
    setMarginRight(10);
    setMarginBottom(10);
    setCornerRadius(10);
    setBorderThickness(2);
    setShadowType(brls::ShadowType::GENERIC);
    setFocusable(!title_.title_id.empty());
    setHideHighlight(true);

    auto* image = new brls::Image();
    image->setWidth(202);
    image->setHeight(303);
    image->setCornerRadius(7);
    image->setMarginBottom(6);
    image->setBackgroundColor(palette.surface_alt);
    image->setScalingType(brls::ImageScalingType::FIT);
    image->setImageFromRes("img/platform/xbox.png");
    addView(image);

    auto* name = new brls::Label();
    name->setText(title_.name.empty() ? title_.title_id : title_.name);
    name->setFontSize(16);
    name->setTextColor(palette.text);
    name->setSingleLine(true);
    addView(name);

    std::string metadata = title_.publisher;
    if (title_.is_recent) {
        metadata = metadata.empty()
            ? brls::getStr("lunarnx/main/badge_recent")
            : metadata + "  /  " + brls::getStr("lunarnx/main/badge_recent");
    } else if (is_new) {
        metadata = metadata.empty()
            ? brls::getStr("lunarnx/main/badge_new")
            : metadata + "  /  " + brls::getStr("lunarnx/main/badge_new");
    }
    auto* subtitle = makeMutedLabel(metadata, 12);
    subtitle->setSingleLine(true);
    addView(subtitle);

    if (!title_.image_url.empty()) {
        PosterLoader::instance().load(image, title_.image_url, poster_batch);
    }

    registerClickAction([this, on_select = std::move(on_select)](brls::View*) {
        if (on_select) on_select(title_);
        return true;
    });
    updateChrome(false);
}

void CloudGameCard::onFocusGained() {
    brls::Box::onFocusGained();
    updateChrome(true);
}

void CloudGameCard::onFocusLost() {
    brls::Box::onFocusLost();
    updateChrome(false);
}

void CloudGameCard::updateChrome(bool focused) {
    const auto& palette = uiPalette();
    setBackgroundColor(focused ? palette.surface_alt : palette.surface);
    setBorderColor(focused ? palette.accent : palette.border);
}

} // namespace lunar::ui
#endif
