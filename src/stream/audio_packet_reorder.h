#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lunar::stream {

struct EncodedAudioPacket {
    std::vector<uint8_t> data;
    uint16_t sequence = 0;
    uint64_t timestamp = 0;
    uint32_t generation = 0;
};

struct AudioReorderAction {
    enum class Type {
        Packet,
        Missing,
    };

    Type type = Type::Packet;
    uint16_t sequence = 0;
    EncodedAudioPacket packet;
};

class AudioPacketReorder {
public:
    std::vector<AudioReorderAction> push(EncodedAudioPacket packet);
    void reset();

private:
    static constexpr size_t kReorderWindow = 4;
    static constexpr uint16_t kMaxPlcFrames = 3;
    static constexpr int16_t kResetGap = 64;

    static int16_t sequenceDistance(uint16_t from, uint16_t to);
    void drainReady(std::vector<AudioReorderAction>& actions);

    bool have_expected_ = false;
    uint16_t expected_sequence_ = 0;
    std::vector<EncodedAudioPacket> pending_;
};

} // namespace lunar::stream
