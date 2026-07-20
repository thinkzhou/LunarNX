#include "audio_packet_reorder.h"

#include <algorithm>
#include <limits>

namespace lunar::stream {

int16_t AudioPacketReorder::sequenceDistance(uint16_t from, uint16_t to) {
    return static_cast<int16_t>(static_cast<uint16_t>(to - from));
}

void AudioPacketReorder::reset() {
    have_expected_ = false;
    expected_sequence_ = 0;
    pending_.clear();
}

void AudioPacketReorder::drainReady(std::vector<AudioReorderAction>& actions) {
    while (true) {
        auto ready = std::find_if(pending_.begin(), pending_.end(), [this](const auto& packet) {
            return packet.sequence == expected_sequence_;
        });
        if (ready == pending_.end()) return;

        AudioReorderAction action;
        action.type = AudioReorderAction::Type::Packet;
        action.sequence = ready->sequence;
        action.packet = std::move(*ready);
        pending_.erase(ready);
        actions.push_back(std::move(action));
        expected_sequence_ = static_cast<uint16_t>(expected_sequence_ + 1);
    }
}

std::vector<AudioReorderAction> AudioPacketReorder::push(EncodedAudioPacket packet) {
    std::vector<AudioReorderAction> actions;
    if (!have_expected_) {
        have_expected_ = true;
        expected_sequence_ = packet.sequence;
    }

    const int16_t distance = sequenceDistance(expected_sequence_, packet.sequence);
    if (distance < 0) return actions;
    if (distance > kResetGap) {
        reset();
        have_expected_ = true;
        expected_sequence_ = packet.sequence;
    }

    const auto duplicate = std::find_if(
        pending_.begin(), pending_.end(), [&packet](const auto& queued) {
            return queued.sequence == packet.sequence;
        });
    if (duplicate != pending_.end()) return actions;

    pending_.push_back(std::move(packet));
    drainReady(actions);

    while (pending_.size() >= kReorderWindow) {
        int16_t nearest = std::numeric_limits<int16_t>::max();
        for (const auto& queued : pending_) {
            const int16_t queued_distance =
                sequenceDistance(expected_sequence_, queued.sequence);
            if (queued_distance > 0 && queued_distance < nearest) {
                nearest = queued_distance;
            }
        }
        if (nearest == std::numeric_limits<int16_t>::max()) break;

        const uint16_t plc_count = std::min<uint16_t>(
            static_cast<uint16_t>(nearest), kMaxPlcFrames);
        for (uint16_t i = 0; i < plc_count; i++) {
            AudioReorderAction missing;
            missing.type = AudioReorderAction::Type::Missing;
            missing.sequence = static_cast<uint16_t>(expected_sequence_ + i);
            actions.push_back(std::move(missing));
        }
        expected_sequence_ = static_cast<uint16_t>(expected_sequence_ + nearest);
        drainReady(actions);
    }
    return actions;
}

} // namespace lunar::stream
