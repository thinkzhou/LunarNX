#pragma once

#ifdef __SWITCH__

#include "qr_code.h"

#include <borealis.hpp>

namespace lunar::ui {

class QrCodeView : public brls::View {
public:
    explicit QrCodeView(float size = 240.0f);
    ~QrCodeView() override;

    void setQrCode(QrCode qr);
    void clearQrCode();
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    void releaseTexture();

    QrBitmap bitmap_;
    int texture_ = -1;
};

} // namespace lunar::ui

#endif
