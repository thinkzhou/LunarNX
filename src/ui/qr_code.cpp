#include "qr_code.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lunar::ui {
namespace {

constexpr int QR_VERSION = 3;
constexpr int QR_SIZE = QR_VERSION * 4 + 17;
constexpr int DATA_CODEWORDS = 55;
constexpr int ECC_CODEWORDS = 15;
constexpr int TOTAL_CODEWORDS = DATA_CODEWORDS + ECC_CODEWORDS;

class BitBuffer {
public:
    void append(uint32_t value, int bit_count) {
        for (int i = bit_count - 1; i >= 0; --i) {
            bits_.push_back(static_cast<uint8_t>((value >> i) & 1U));
        }
    }

    size_t size() const { return bits_.size(); }

    std::vector<uint8_t> toCodewords() const {
        std::vector<uint8_t> out((bits_.size() + 7) / 8, 0);
        for (size_t i = 0; i < bits_.size(); ++i) {
            out[i / 8] |= static_cast<uint8_t>(bits_[i] << (7 - (i % 8)));
        }
        return out;
    }

private:
    std::vector<uint8_t> bits_;
};

class QrBuilder {
public:
    QrBuilder() : modules_(QR_SIZE * QR_SIZE, 0), reserved_(QR_SIZE * QR_SIZE, 0) {}

    QrCode build(const std::string& text) {
        if (text.size() > 53) return {};

        drawFunctionPatterns();
        std::vector<uint8_t> codewords = makeCodewords(text);
        drawCodewords(codewords);
        drawFormatBits();

        QrCode qr;
        qr.size = QR_SIZE;
        qr.modules = std::move(modules_);
        return qr;
    }

private:
    std::vector<uint8_t> modules_;
    std::vector<uint8_t> reserved_;

    static int index(int x, int y) { return y * QR_SIZE + x; }

    void setModule(int x, int y, bool dark, bool reserved = true) {
        if (x < 0 || y < 0 || x >= QR_SIZE || y >= QR_SIZE) return;
        modules_[index(x, y)] = dark ? 1 : 0;
        if (reserved) reserved_[index(x, y)] = 1;
    }

    bool isReserved(int x, int y) const {
        return reserved_[index(x, y)] != 0;
    }

    void drawFinder(int x, int y) {
        for (int dy = -1; dy <= 7; ++dy) {
            for (int dx = -1; dx <= 7; ++dx) {
                setModule(x + dx, y + dy, false);
            }
        }

        for (int dy = 0; dy < 7; ++dy) {
            for (int dx = 0; dx < 7; ++dx) {
                bool border = dx == 0 || dx == 6 || dy == 0 || dy == 6;
                bool center = dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4;
                setModule(x + dx, y + dy, border || center);
            }
        }
    }

    void drawAlignment(int cx, int cy) {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                int dist = dx > 0 ? dx : -dx;
                int ady = dy > 0 ? dy : -dy;
                if (ady > dist) dist = ady;
                setModule(cx + dx, cy + dy, dist != 1);
            }
        }
    }

    void reserveFormatBits() {
        for (int i = 0; i <= 5; ++i) setModule(8, i, false);
        setModule(8, 7, false);
        setModule(8, 8, false);
        setModule(7, 8, false);
        for (int i = 9; i < 15; ++i) setModule(14 - i, 8, false);

        for (int i = 0; i < 8; ++i) setModule(QR_SIZE - 1 - i, 8, false);
        for (int i = 8; i < 15; ++i) setModule(8, QR_SIZE - 15 + i, false);
    }

    void drawFunctionPatterns() {
        drawFinder(0, 0);
        drawFinder(QR_SIZE - 7, 0);
        drawFinder(0, QR_SIZE - 7);

        for (int i = 0; i < QR_SIZE; ++i) {
            if (!isReserved(i, 6)) setModule(i, 6, i % 2 == 0);
            if (!isReserved(6, i)) setModule(6, i, i % 2 == 0);
        }

        drawAlignment(22, 22);
        setModule(8, QR_VERSION * 4 + 9, true);
        reserveFormatBits();
    }

    static uint8_t gfMul(uint8_t x, uint8_t y) {
        int z = 0;
        for (int i = 7; i >= 0; --i) {
            z = (z << 1) ^ ((z >> 7) * 0x11D);
            if (((y >> i) & 1) != 0) z ^= x;
        }
        return static_cast<uint8_t>(z);
    }

    static uint8_t gfPow(int exp) {
        uint8_t value = 1;
        for (int i = 0; i < exp; ++i) value = gfMul(value, 2);
        return value;
    }

    static std::vector<uint8_t> reedSolomonGenerator() {
        std::vector<uint8_t> gen{1};
        for (int i = 0; i < ECC_CODEWORDS; ++i) {
            std::vector<uint8_t> next(gen.size() + 1, 0);
            uint8_t root = gfPow(i);
            for (size_t j = 0; j < gen.size(); ++j) {
                next[j] ^= gen[j];
                next[j + 1] ^= gfMul(gen[j], root);
            }
            gen = std::move(next);
        }
        return gen;
    }

    static std::vector<uint8_t> reedSolomonRemainder(const std::vector<uint8_t>& data) {
        std::vector<uint8_t> gen = reedSolomonGenerator();
        std::vector<uint8_t> result(ECC_CODEWORDS, 0);
        for (uint8_t value : data) {
            uint8_t factor = value ^ result[0];
            for (int i = 0; i < ECC_CODEWORDS - 1; ++i) {
                result[i] = result[i + 1];
            }
            result[ECC_CODEWORDS - 1] = 0;
            for (int i = 0; i < ECC_CODEWORDS; ++i) {
                result[i] ^= gfMul(gen[static_cast<size_t>(i + 1)], factor);
            }
        }
        return result;
    }

    static std::vector<uint8_t> makeCodewords(const std::string& text) {
        BitBuffer bits;
        bits.append(0x4, 4); // byte mode
        bits.append(static_cast<uint32_t>(text.size()), 8);
        for (unsigned char c : text) bits.append(c, 8);

        const size_t capacity_bits = DATA_CODEWORDS * 8;
        size_t terminator = capacity_bits - bits.size();
        if (terminator > 4) terminator = 4;
        bits.append(0, static_cast<int>(terminator));
        while (bits.size() % 8 != 0) bits.append(0, 1);

        std::vector<uint8_t> data = bits.toCodewords();
        for (uint8_t pad = 0xEC; data.size() < DATA_CODEWORDS; pad ^= 0xFD) {
            data.push_back(pad);
        }

        std::vector<uint8_t> ecc = reedSolomonRemainder(data);
        std::vector<uint8_t> codewords;
        codewords.reserve(TOTAL_CODEWORDS);
        codewords.insert(codewords.end(), data.begin(), data.end());
        codewords.insert(codewords.end(), ecc.begin(), ecc.end());
        return codewords;
    }

    static bool mask(int x, int y) {
        return ((x + y) & 1) == 0;
    }

    void drawCodewords(const std::vector<uint8_t>& codewords) {
        int bit_index = 0;
        bool upward = true;
        for (int right = QR_SIZE - 1; right >= 1; right -= 2) {
            if (right == 6) --right;
            for (int vert = 0; vert < QR_SIZE; ++vert) {
                int y = upward ? QR_SIZE - 1 - vert : vert;
                for (int j = 0; j < 2; ++j) {
                    int x = right - j;
                    if (isReserved(x, y)) continue;
                    bool bit = false;
                    if (bit_index < static_cast<int>(codewords.size() * 8)) {
                        bit = ((codewords[static_cast<size_t>(bit_index / 8)] >>
                               (7 - (bit_index % 8))) & 1) != 0;
                    }
                    setModule(x, y, bit ^ mask(x, y), false);
                    ++bit_index;
                }
            }
            upward = !upward;
        }
    }

    static uint16_t formatBits() {
        int data = 0b01000; // error correction L, mask 0
        int value = data << 10;
        constexpr int generator = 0x537;
        for (int i = 14; i >= 10; --i) {
            if (((value >> i) & 1) != 0) {
                value ^= generator << (i - 10);
            }
        }
        return static_cast<uint16_t>(((data << 10) | value) ^ 0x5412);
    }

    void drawFormatBits() {
        uint16_t bits = formatBits();
        auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };

        for (int i = 0; i <= 5; ++i) setModule(8, i, bit(i));
        setModule(8, 7, bit(6));
        setModule(8, 8, bit(7));
        setModule(7, 8, bit(8));
        for (int i = 9; i < 15; ++i) setModule(14 - i, 8, bit(i));

        for (int i = 0; i < 8; ++i) setModule(QR_SIZE - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i) setModule(8, QR_SIZE - 15 + i, bit(i));
        setModule(8, QR_VERSION * 4 + 9, true);
    }
};

} // namespace

QrCode makeQrCode(const std::string& text) {
    return QrBuilder().build(text);
}

} // namespace lunar::ui
