#include "qr_code.h"

#include "../../lib/qrcodegen/qrcodegen.hpp"

#include <exception>

namespace lunar::ui {

QrCode makeQrCode(const std::string& text) {
    if (text.empty()) return {};

    try {
        qrcodegen::QrCode encoded = qrcodegen::QrCode::encodeText(
            text.c_str(), qrcodegen::QrCode::Ecc::LOW);

        QrCode result;
        result.size = encoded.getSize();
        result.modules.reserve(static_cast<size_t>(result.size * result.size));
        for (int y = 0; y < result.size; ++y) {
            for (int x = 0; x < result.size; ++x) {
                result.modules.push_back(encoded.getModule(x, y) ? 1 : 0);
            }
        }
        return result;
    } catch (const std::exception&) {
        return {};
    }
}

QrBitmap makeQrBitmap(const QrCode& qr, int quiet_zone) {
    if (qr.empty() || quiet_zone < 0) return {};

    QrBitmap bitmap;
    bitmap.size = qr.size + quiet_zone * 2;
    bitmap.rgba.assign(static_cast<size_t>(bitmap.size * bitmap.size * 4), 0xFF);

    for (int y = 0; y < qr.size; ++y) {
        for (int x = 0; x < qr.size; ++x) {
            if (!qr.at(x, y)) continue;
            const int px = x + quiet_zone;
            const int py = y + quiet_zone;
            const size_t offset = static_cast<size_t>((py * bitmap.size + px) * 4);
            bitmap.rgba[offset + 0] = 0;
            bitmap.rgba[offset + 1] = 0;
            bitmap.rgba[offset + 2] = 0;
        }
    }
    return bitmap;
}

} // namespace lunar::ui
