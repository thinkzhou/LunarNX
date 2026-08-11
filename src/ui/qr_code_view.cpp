#ifdef __SWITCH__

#include "qr_code_view.h"

#include <algorithm>
#include <utility>

namespace lunar::ui {

QrCodeView::QrCodeView(float size) {
    this->setDimensions(size, size);
}

QrCodeView::~QrCodeView() {
    releaseTexture();
}

void QrCodeView::releaseTexture() {
    if (texture_ > 0) {
        nvgDeleteImage(brls::Application::getNVGContext(), texture_);
    }
    texture_ = -1;
}

void QrCodeView::setQrCode(QrCode qr) {
    releaseTexture();
    bitmap_ = makeQrBitmap(qr);
    this->setVisibility(bitmap_.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    this->invalidate();
}

void QrCodeView::clearQrCode() {
    releaseTexture();
    bitmap_ = {};
    this->setVisibility(brls::Visibility::GONE);
    this->invalidate();
}

void QrCodeView::draw(NVGcontext* vg, float x, float y, float width, float height,
                      brls::Style style, brls::FrameContext* ctx) {
    (void)style;
    (void)ctx;
    if (bitmap_.empty()) return;

    if (texture_ < 0) {
        texture_ = nvgCreateImageRGBA(vg, bitmap_.size, bitmap_.size,
                                      NVG_IMAGE_NEAREST, bitmap_.rgba.data());
        if (texture_ <= 0) return;
    }

    const float side = std::min(width, height);
    const float scale = static_cast<float>(static_cast<int>(side / bitmap_.size));
    if (scale < 1.0f) return;
    const float actual = scale * bitmap_.size;
    const float ox = x + (width - actual) * 0.5f;
    const float oy = y + (height - actual) * 0.5f;

    NVGpaint image = nvgImagePattern(vg, ox, oy, actual, actual, 0.0f, texture_, 1.0f);
    nvgBeginPath(vg);
    nvgRect(vg, ox, oy, actual, actual);
    nvgFillPaint(vg, image);
    nvgFill(vg);
}

} // namespace lunar::ui

#endif
