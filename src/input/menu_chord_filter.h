#pragma once
#include <cstdint>
namespace lunar::input {
// Delay a single menu key briefly to distinguish it from the reserved chord.
class MenuChordFilter {
public:
    uint64_t update(uint64_t buttons, uint64_t mask, uint64_t ms) {
        const auto keys = buttons & mask;
        if (keys == mask || blocked_) {
            blocked_ = keys != 0;
            pending_ = pulse_ = 0;
            return buttons & ~mask;
        }
        if (pending_ && keys != pending_) {
            // Preserve a short single-key tap after release.
            if (ms - since_ < 120) { pulse_ = pending_; pulse_until_ = ms + 40; }
            pending_ = 0;
        }
        if (keys && !pending_) { pending_ = keys; since_ = ms; }
        uint64_t output = (pending_ && ms - since_ >= 120) ? pending_ : 0;
        if (ms < pulse_until_) output |= pulse_;
        return (buttons & ~mask) | output;
    }
private:
    uint64_t pending_ = 0, since_ = 0, pulse_ = 0, pulse_until_ = 0;
    bool blocked_ = false;
};
}
