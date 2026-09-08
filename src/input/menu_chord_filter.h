#pragma once
#include <cstdint>
namespace lunar::input {
// Delay a single menu key briefly to distinguish it from the reserved chord.
class MenuChordFilter {
public:
    uint64_t replayedButtons() const { return replayed_; }
    uint64_t update(uint64_t buttons, uint64_t mask, uint64_t ms, uint64_t mapped_chord = 0) {
        replayed_ = 0;
        const auto keys = buttons & mask;
        if (keys == mask || blocked_) {
            blocked_ = keys != 0;
            pending_ = pulse_ = latched_ = 0;
            published_ = false;
            return buttons & ~mask;
        }
        // A configured game chord is an intentional action, not a partial menu
        // shortcut. Keep its components together and suppress staggered releases.
        if (mapped_chord) {
            latched_ |= mapped_chord;
            pending_ = pulse_ = 0;
            published_ = false;
            return buttons & ~(latched_ & ~mapped_chord);
        }
        if (latched_) {
            const auto held = buttons & latched_;
            buttons &= ~latched_;
            if (!held) latched_ = 0;
            return buttons;
        }
        if (pending_ && keys != pending_) {
            // Preserve a short single-key tap after release.
            if (!published_) { pulse_ = pending_; pulse_until_ = ms + 40; }
            pending_ = 0;
        }
        if (keys && !pending_) { pending_ = keys; since_ = ms; published_ = false; }
        uint64_t output = (pending_ && ms - since_ >= 120) ? pending_ : 0;
        if (output) published_ = true;
        if (ms < pulse_until_) replayed_ = pulse_;
        return (buttons & ~mask) | output;
    }
private:
    uint64_t pending_ = 0, since_ = 0, pulse_ = 0, pulse_until_ = 0;
    uint64_t latched_ = 0, replayed_ = 0;
    bool published_ = false;
    bool blocked_ = false;
};
}
