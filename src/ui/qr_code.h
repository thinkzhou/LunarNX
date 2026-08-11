#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lunar::ui {

struct QrCode {
    int size = 0;
    std::vector<uint8_t> modules;

    bool empty() const { return size <= 0 || modules.empty(); }
    bool at(int x, int y) const {
        if (x < 0 || y < 0 || x >= size || y >= size) return false;
        return modules[static_cast<size_t>(y * size + x)] != 0;
    }
};

struct QrBitmap {
    int size = 0;
    std::vector<uint8_t> rgba;

    bool empty() const { return size <= 0 || rgba.empty(); }
};

QrCode makeQrCode(const std::string& text);
QrBitmap makeQrBitmap(const QrCode& qr, int quiet_zone = 4);

} // namespace lunar::ui
