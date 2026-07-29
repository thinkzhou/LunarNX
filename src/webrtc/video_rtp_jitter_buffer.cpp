#include "video_rtp_jitter_buffer.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <deque>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace lunar::webrtc {

namespace {

constexpr size_t kSequenceWindow = 4096;
constexpr uint32_t kMaxNackGap = 255;
constexpr uint64_t kNackWindowMs = 50;
constexpr uint32_t kMaxNacksPerWindow = 8;
constexpr size_t kMaxPendingNackRanges = 8;
constexpr size_t kMaxRtpPayloadBytes = 2048;
constexpr uint8_t kStartCode[] = {0x00, 0x00, 0x00, 0x01};

bool timestampNewer(uint32_t left, uint32_t right) {
    const uint32_t delta = left - right;
    return delta != 0 && delta < 0x80000000u;
}

struct ParsedRtp {
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    uint32_t ssrc = 0;
#endif
    bool marker = false;
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;
};

bool parseRtp(const uint8_t* packet, size_t size, ParsedRtp& parsed) {
    if (!packet || size < 12 || (packet[0] >> 6) != 2) return false;

    const size_t csrc_count = packet[0] & 0x0f;
    size_t offset = 12 + csrc_count * 4;
    if (offset > size) return false;

    if ((packet[0] & 0x10) != 0) {
        if (offset + 4 > size) return false;
        const size_t extension_words =
            (static_cast<size_t>(packet[offset + 2]) << 8) |
            packet[offset + 3];
        if (extension_words > (size - offset - 4) / 4) return false;
        offset += 4 + extension_words * 4;
    }
    if (offset > size) return false;

    size_t payload_size = size - offset;
    if ((packet[0] & 0x20) != 0) {
        if (payload_size == 0) return false;
        const size_t padding = packet[size - 1];
        if (padding == 0 || padding > payload_size) return false;
        payload_size -= padding;
    }

    parsed.sequence = static_cast<uint16_t>(packet[2] << 8 | packet[3]);
    parsed.timestamp =
        (static_cast<uint32_t>(packet[4]) << 24) |
        (static_cast<uint32_t>(packet[5]) << 16) |
        (static_cast<uint32_t>(packet[6]) << 8) |
        packet[7];
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    parsed.ssrc =
        (static_cast<uint32_t>(packet[8]) << 24) |
        (static_cast<uint32_t>(packet[9]) << 16) |
        (static_cast<uint32_t>(packet[10]) << 8) |
        packet[11];
#endif
    parsed.marker = (packet[1] & 0x80) != 0;
    parsed.payload = packet + offset;
    parsed.payload_size = payload_size;
    return true;
}

uint8_t naluType(const uint8_t* payload, size_t size) {
    return size > 0 ? payload[0] & 0x1f : 0;
}

bool isPartitionHead(const uint8_t* payload, size_t size) {
    const uint8_t type = naluType(payload, size);
    if (type == 28) return size >= 2 && (payload[1] & 0x80) != 0;
    return type >= 1 && type <= 24;
}

bool appendBytes(std::vector<uint8_t>& output,
                 const uint8_t* data,
                 size_t size) {
    if (size > VideoRtpJitterBuffer::kMaxAccessUnitBytes - output.size()) {
        return false;
    }
    output.insert(output.end(), data, data + size);
    return true;
}

enum class DepacketizeResult {
    Ok,
    Invalid,
    Unsupported,
    Overflow,
};

struct H264AssemblyState {
    bool fu_active = false;
    uint8_t fu_nalu_header = 0;
};

DepacketizeResult depacketize(const uint8_t* payload,
                              size_t size,
                              std::vector<uint8_t>& output,
                              H264AssemblyState& state) {
    if (!payload || size == 0) return DepacketizeResult::Ok;
    const uint8_t type = naluType(payload, size);
    if (type >= 1 && type <= 23) {
        if (state.fu_active) return DepacketizeResult::Invalid;
        return appendBytes(output, kStartCode, sizeof(kStartCode)) &&
                       appendBytes(output, payload, size)
                   ? DepacketizeResult::Ok
                   : DepacketizeResult::Overflow;
    }
    if (type == 24) {
        if (state.fu_active || size < 4) return DepacketizeResult::Invalid;
        size_t position = 1;
        while (position + 2 <= size) {
            const size_t nalu_size =
                (static_cast<size_t>(payload[position]) << 8) |
                payload[position + 1];
            position += 2;
            if (nalu_size == 0 || nalu_size > size - position) {
                return DepacketizeResult::Invalid;
            }
            const uint8_t aggregated_type = naluType(payload + position,
                                                      nalu_size);
            if (aggregated_type == 0 || aggregated_type >= 24) {
                return DepacketizeResult::Unsupported;
            }
            if (!appendBytes(output, kStartCode, sizeof(kStartCode)) ||
                !appendBytes(output, payload + position, nalu_size)) {
                return DepacketizeResult::Overflow;
            }
            position += nalu_size;
        }
        return position == size ? DepacketizeResult::Ok
                                : DepacketizeResult::Invalid;
    }
    if (type == 28) {
        if (size <= 2) return DepacketizeResult::Invalid;
        const uint8_t fu_header = payload[1];
        const bool start = (fu_header & 0x80) != 0;
        const bool end = (fu_header & 0x40) != 0;
        const bool reserved = (fu_header & 0x20) != 0;
        const uint8_t fragmented_type = fu_header & 0x1f;
        if (reserved || fragmented_type == 0 || fragmented_type >= 24 ||
            (start && end)) {
            return DepacketizeResult::Invalid;
        }
        const uint8_t reconstructed =
            static_cast<uint8_t>((payload[0] & 0xe0) | fragmented_type);
        if (start) {
            if (state.fu_active) return DepacketizeResult::Invalid;
            if (!appendBytes(output, kStartCode, sizeof(kStartCode)) ||
                !appendBytes(output, &reconstructed, 1)) {
                return DepacketizeResult::Overflow;
            }
            state.fu_active = true;
            state.fu_nalu_header = reconstructed;
        } else if (!state.fu_active ||
                   state.fu_nalu_header != reconstructed) {
            return DepacketizeResult::Invalid;
        }
        if (!appendBytes(output, payload + 2, size - 2)) {
            return DepacketizeResult::Overflow;
        }
        if (end) state.fu_active = false;
        return DepacketizeResult::Ok;
    }
    return DepacketizeResult::Unsupported;
}

bool containsIdr(const std::vector<uint8_t>& access_unit) {
    for (size_t i = 0; i + 4 < access_unit.size(); ++i) {
        if (access_unit[i] == 0 && access_unit[i + 1] == 0 &&
            access_unit[i + 2] == 0 && access_unit[i + 3] == 1 &&
            (access_unit[i + 4] & 0x1f) == 5) {
            return true;
        }
    }
    return false;
}

} // namespace

struct VideoRtpJitterBuffer::Impl {
    struct Packet {
        uint16_t sequence = 0;
        uint32_t extended_sequence = 0;
        bool marker = false;
        size_t payload_offset = 0;
        size_t payload_size = 0;
    };

    struct Frame {
        uint32_t timestamp = 0;
        uint64_t first_seen_ms = 0;
        uint64_t last_progress_ms = 0;
        bool marker_seen = false;
        std::vector<Packet> packets;
        std::vector<uint8_t> payload_storage;
    };

    enum class AssembleResult {
        Complete,
        CompleteDiscontinuous,
        Incomplete,
        Invalid,
        Overflow,
    };

    struct SequenceUpdate {
        uint32_t extended_sequence = 0;
        bool duplicate = false;
    };

    std::deque<Frame> frames;
    std::bitset<kSequenceWindow> received_window;
    uint64_t hold_ms = kDefaultHoldMs;
    uint64_t recovery_hold_ms = kDefaultRecoveryHoldMs;
    size_t buffered_bytes = 0;
    size_t buffered_packets = 0;

    bool waiting_keyframe = true;
    bool have_sequence = false;
    uint16_t max_sequence = 0;
    uint32_t highest_sequence = 0;
    uint32_t base_sequence = 0;
    uint32_t unique_received = 0;
    uint32_t expected_prior = 0;
    uint32_t received_prior = 0;

    bool have_last_timestamp = false;
    uint32_t last_timestamp = 0;
    bool have_last_consumed_sequence = false;
    uint32_t last_consumed_sequence = 0;

    bool recovery_notified = false;
    bool decoder_reset_notified = false;
    uint64_t last_recovery_notify_ms = 0;
    bool soft_recovery_active = false;
    uint64_t last_soft_loss_ms = 0;
    uint32_t soft_losses_in_window = 0;
    bool have_nack_window = false;
    uint64_t nack_window_started_ms = 0;
    uint32_t nacks_in_window = 0;
    struct MissingRange {
        uint32_t cursor = 0;
        uint32_t end = 0;
        uint32_t timestamp = 0;
        bool recovery_candidate = false;
        bool recovery_authorized = false;
    };
    std::array<MissingRange, kMaxPendingNackRanges> pending_nack_ranges{};
    size_t pending_nack_head = 0;
    size_t pending_nack_count = 0;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    bool have_ssrc = false;
#endif

    VideoRtpJitterStats counters;

    void clearFrames() {
        frames.clear();
        buffered_bytes = 0;
        buffered_packets = 0;
    }

    void reset() {
        clearFrames();
        received_window.reset();
        waiting_keyframe = true;
        have_sequence = false;
        max_sequence = 0;
        highest_sequence = 0;
        base_sequence = 0;
        unique_received = 0;
        expected_prior = 0;
        received_prior = 0;
        have_last_timestamp = false;
        last_timestamp = 0;
        have_last_consumed_sequence = false;
        last_consumed_sequence = 0;
        recovery_notified = false;
        decoder_reset_notified = false;
        last_recovery_notify_ms = 0;
        soft_recovery_active = false;
        last_soft_loss_ms = 0;
        soft_losses_in_window = 0;
        have_nack_window = false;
        nack_window_started_ms = 0;
        nacks_in_window = 0;
        pending_nack_head = 0;
        pending_nack_count = 0;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        have_ssrc = false;
#endif
        counters = {};
    }

    bool wasReceived(uint32_t extended_sequence) const {
        if (!have_sequence || extended_sequence > highest_sequence) return false;
        const uint32_t distance = highest_sequence - extended_sequence;
        return distance < kSequenceWindow && received_window.test(distance);
    }

    void updateMissingPackets() {
        if (!have_sequence) {
            counters.missing_packets = 0;
            return;
        }
        const uint32_t expected = highest_sequence - base_sequence + 1;
        counters.missing_packets =
            expected > unique_received ? expected - unique_received : 0;
        counters.highest_sequence = highest_sequence;
    }

    MissingRange& pendingNackFront() {
        return pending_nack_ranges[pending_nack_head];
    }

    void popPendingNackRange() {
        pending_nack_head = (pending_nack_head + 1) % kMaxPendingNackRanges;
        pending_nack_count--;
    }

    void discardCompletedPendingNackRanges() {
        while (pending_nack_count > 0 &&
               pendingNackFront().cursor >= pendingNackFront().end) {
            popPendingNackRange();
        }
    }

    void clearPendingNacks() {
        pending_nack_head = 0;
        pending_nack_count = 0;
    }

    template <typename Keep>
    void filterPendingNacks(Keep keep) {
        std::array<MissingRange, kMaxPendingNackRanges> filtered{};
        size_t count = 0;
        for (size_t index = 0; index < pending_nack_count; ++index) {
            auto range = pending_nack_ranges[
                (pending_nack_head + index) % kMaxPendingNackRanges];
            if (range.cursor < range.end && keep(range)) {
                filtered[count++] = range;
            }
        }
        pending_nack_ranges = filtered;
        pending_nack_head = 0;
        pending_nack_count = count;
    }

    void drainPendingNacks(uint64_t now_ms, const NackCallback& nack) {
        discardCompletedPendingNackRanges();
        if (!nack || pending_nack_count == 0) return;
        if (!have_nack_window || now_ms < nack_window_started_ms ||
            now_ms - nack_window_started_ms >= kNackWindowMs) {
            have_nack_window = true;
            nack_window_started_ms = now_ms;
            nacks_in_window = 0;
        }
        while (pending_nack_count > 0 &&
               nacks_in_window < kMaxNacksPerWindow) {
            auto& range = pendingNackFront();
            if (range.recovery_candidate && !range.recovery_authorized) {
                return;
            }
            while (range.cursor < range.end && wasReceived(range.cursor)) {
                range.cursor++;
            }
            if (range.cursor >= range.end) {
                popPendingNackRange();
                continue;
            }

            const uint32_t pid_extended = range.cursor;
            const uint16_t pid = static_cast<uint16_t>(pid_extended);
            uint16_t blp = 0;
            const uint32_t next_cursor = pid_extended +
                std::min<uint32_t>(17, range.end - pid_extended);
            for (uint32_t sequence = pid_extended + 1;
                 sequence < next_cursor;
                 ++sequence) {
                if (!wasReceived(sequence)) {
                    blp |= static_cast<uint16_t>(1u << (sequence - pid_extended - 1));
                }
            }
            try {
                if (!nack(pid, blp)) return;
                range.cursor = next_cursor;
                counters.nacks++;
                nacks_in_window++;
            } catch (...) {
                return;
            }
        }
    }

    void sendNacks(uint32_t first_missing,
                   uint32_t end,
                   uint32_t timestamp,
                   uint64_t now_ms,
                   const NackCallback& nack,
                   bool recovery_authorized) {
        if (!nack || end <= first_missing ||
            end - first_missing > kMaxNackGap) {
            return;
        }
        discardCompletedPendingNackRanges();
        if (pending_nack_count >= kMaxPendingNackRanges) return;
        const size_t tail =
            (pending_nack_head + pending_nack_count) % kMaxPendingNackRanges;
        pending_nack_ranges[tail] = {
            first_missing,
            end,
            timestamp,
            waiting_keyframe,
            !waiting_keyframe || recovery_authorized,
        };
        pending_nack_count++;
        drainPendingNacks(now_ms, nack);
    }

    SequenceUpdate noteSequence(uint16_t sequence,
                                uint32_t timestamp,
                                uint64_t now_ms,
                                const NackCallback& nack,
                                bool recovery_authorized) {
        SequenceUpdate update;
        if (!have_sequence) {
            have_sequence = true;
            max_sequence = sequence;
            highest_sequence = sequence;
            base_sequence = sequence;
            unique_received = 1;
            received_window.set(0);
            update.extended_sequence = sequence;
            updateMissingPackets();
            return update;
        }

        uint32_t extended = (highest_sequence & 0xffff0000u) | sequence;
        const uint16_t highest_low = static_cast<uint16_t>(highest_sequence);
        if (sequence < highest_low &&
            static_cast<uint16_t>(highest_low - sequence) > 0x8000u) {
            extended += 0x10000u;
        } else if (sequence > highest_low &&
                   static_cast<uint16_t>(sequence - highest_low) > 0x8000u &&
                   extended >= 0x10000u) {
            extended -= 0x10000u;
        }
        update.extended_sequence = extended;

        if (extended > highest_sequence) {
            const uint32_t previous_highest = highest_sequence;
            const uint32_t delta = extended - highest_sequence;
            if (delta >= kSequenceWindow) {
                received_window.reset();
            } else {
                received_window <<= delta;
            }
            received_window.set(0);
            highest_sequence = extended;
            max_sequence = sequence;
            unique_received++;
            if (delta > 1) {
                counters.sequence_gaps++;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
                counters.last_gap_packets = delta - 1;
#endif
                sendNacks(previous_highest + 1,
                          extended,
                          timestamp,
                          now_ms,
                          nack,
                          recovery_authorized);
            }
        } else {
            const uint32_t distance = highest_sequence - extended;
            if (distance >= kSequenceWindow || received_window.test(distance)) {
                update.duplicate = true;
                return update;
            }
            received_window.set(distance);
            unique_received++;
        }
        updateMissingPackets();
        return update;
    }

    Frame* findFrame(uint32_t timestamp) {
        for (auto& frame : frames) {
            if (frame.timestamp == timestamp) return &frame;
        }
        return nullptr;
    }

    Frame* createFrame(uint32_t timestamp, uint64_t now_ms) {
        if (frames.size() >= kMaxBufferedFrames) return nullptr;

        Frame frame;
        frame.timestamp = timestamp;
        frame.first_seen_ms = now_ms;
        frame.last_progress_ms = now_ms;
        auto position = frames.begin();
        while (position != frames.end() &&
               timestampNewer(timestamp, position->timestamp)) {
            ++position;
        }
        return &*frames.insert(position, std::move(frame));
    }

    void removeFrontFrame() {
        if (frames.empty()) return;
        buffered_bytes -= frames.front().payload_storage.size();
        buffered_packets -= frames.front().packets.size();
        frames.pop_front();
    }

    static const uint8_t* packetPayload(const Frame& frame,
                                        const Packet& packet) {
        return packet.payload_size > 0
            ? frame.payload_storage.data() + packet.payload_offset
            : nullptr;
    }

    static bool packetMayContainIdr(const uint8_t* payload, size_t size) {
        if (!payload || size == 0) return false;
        const uint8_t type = naluType(payload, size);
        if (type == 5) return true;
        if (type == 28) {
            return size >= 2 && (payload[1] & 0x1f) == 5;
        }
        if (type != 24) return false;

        size_t position = 1;
        while (position + 2 <= size) {
            const size_t nalu_size =
                (static_cast<size_t>(payload[position]) << 8) |
                payload[position + 1];
            position += 2;
            if (nalu_size == 0 || nalu_size > size - position) return false;
            if (naluType(payload + position, nalu_size) == 5) return true;
            position += nalu_size;
        }
        return false;
    }

    static bool packetMayContainRecoveryPoint(const uint8_t* payload,
                                              size_t size) {
        if (!payload || size == 0) return false;
        const uint8_t type = naluType(payload, size);
        if (type == 5 || type == 7 || type == 8) return true;
        if (type == 28) {
            if (size < 2) return false;
            const uint8_t fragmented_type = payload[1] & 0x1f;
            return fragmented_type == 5 || fragmented_type == 7 ||
                   fragmented_type == 8;
        }
        if (type != 24) return false;

        size_t position = 1;
        while (position + 2 <= size) {
            const size_t nalu_size =
                (static_cast<size_t>(payload[position]) << 8) |
                payload[position + 1];
            position += 2;
            if (nalu_size == 0 || nalu_size > size - position) return false;
            const uint8_t aggregated_type = naluType(payload + position,
                                                      nalu_size);
            if (aggregated_type == 5 || aggregated_type == 7 ||
                aggregated_type == 8) {
                return true;
            }
            position += nalu_size;
        }
        return false;
    }

    static bool frameMayContainIdr(const Frame& frame) {
        for (const auto& packet : frame.packets) {
            if (packetMayContainIdr(packetPayload(frame, packet),
                                    packet.payload_size)) {
                return true;
            }
        }
        return false;
    }

    static bool frameMayContainRecoveryPoint(const Frame& frame) {
        for (const auto& packet : frame.packets) {
            if (packetMayContainRecoveryPoint(packetPayload(frame, packet),
                                              packet.payload_size)) {
                return true;
            }
        }
        return false;
    }

    bool frameMayContainIdr(uint32_t timestamp) const {
        for (const auto& frame : frames) {
            if (frame.timestamp == timestamp) return frameMayContainIdr(frame);
        }
        return false;
    }

    void authorizeRecoveryNacks(uint32_t timestamp,
                                uint64_t now_ms,
                                const NackCallback& nack) {
        // Once an IDR fragment is observed, only its timestamp can still
        // repair the decoder. Older tentative gaps are stale P-frames.
        filterPendingNacks([timestamp](const MissingRange& range) {
            return !range.recovery_candidate || range.timestamp == timestamp;
        });
        for (size_t index = 0; index < pending_nack_count; ++index) {
            auto& range = pending_nack_ranges[
                (pending_nack_head + index) % kMaxPendingNackRanges];
            if (range.recovery_candidate && range.timestamp == timestamp) {
                range.recovery_authorized = true;
            }
        }
        drainPendingNacks(now_ms, nack);
    }

#if LUNARNX_DROP_DIAGNOSTIC_LOG
    static bool frameHasPartitionHead(const Frame& frame) {
        for (const auto& packet : frame.packets) {
            if (packet.payload_size > 0) {
                return isPartitionHead(packetPayload(frame, packet),
                                       packet.payload_size);
            }
        }
        return false;
    }
#endif

    static uint64_t elapsedMs(uint64_t now_ms, uint64_t then_ms) {
        return now_ms >= then_ms ? now_ms - then_ms : 0;
    }

    void notifyRecovery(const RecoveryCallback& recovery,
                        bool reset_decoder,
                        uint64_t now_ms,
                        bool force) {
        if (!recovery) return;
        if (!force && recovery_notified &&
            now_ms - last_recovery_notify_ms < 1000) {
            return;
        }
        try {
            recovery(reset_decoder);
            recovery_notified = true;
            last_recovery_notify_ms = now_ms;
        } catch (...) {
        }
    }

    void enterRecovery(const RecoveryCallback& recovery,
                       uint64_t now_ms,
                       uint32_t discarded_timestamp,
                       bool reset_decoder,
                       const char*) {
        const bool was_waiting = waiting_keyframe;
        waiting_keyframe = true;
        // A later buffered IDR may already have a pending range. Keep only
        // those later-frame ranges and wait for that frame's IDR evidence.
        filterPendingNacks([this, discarded_timestamp](MissingRange& range) {
            if (!timestampNewer(range.timestamp, discarded_timestamp)) {
                return false;
            }
            range.recovery_candidate = true;
            range.recovery_authorized = frameMayContainIdr(range.timestamp);
            return true;
        });
        soft_recovery_active = false;
        soft_losses_in_window = 0;
        const bool request_decoder_reset =
            reset_decoder && !decoder_reset_notified;
        if (request_decoder_reset) {
            decoder_reset_notified = true;
            notifyRecovery(recovery, true, now_ms, true);
        } else if (!was_waiting) {
            notifyRecovery(recovery, false, now_ms, true);
        } else {
            notifyRecovery(recovery, false, now_ms, false);
        }
    }

    void requestSoftRecovery(const RecoveryCallback& recovery,
                             uint64_t now_ms) {
        const bool first_request = !soft_recovery_active;
        soft_recovery_active = true;
        notifyRecovery(recovery, false, now_ms, first_request);
    }

    bool softLossRequiresHardRecovery(uint64_t now_ms) {
        constexpr uint64_t kSoftLossWindowMs = 500;
        constexpr uint32_t kSoftLossLimit = 2;
        if (last_soft_loss_ms > 0 &&
            elapsedMs(now_ms, last_soft_loss_ms) <= kSoftLossWindowMs) {
            soft_losses_in_window++;
        } else {
            soft_losses_in_window = 1;
        }
        last_soft_loss_ms = now_ms;
        return soft_losses_in_window >= kSoftLossLimit;
    }

    bool sequenceRangeReceived(uint32_t first, uint32_t last) const {
        if (last < first) return true;
        if (last - first >= kSequenceWindow) return false;
        for (uint32_t sequence = first; sequence <= last; ++sequence) {
            if (!wasReceived(sequence)) return false;
            if (sequence == std::numeric_limits<uint32_t>::max()) break;
        }
        return true;
    }

    AssembleResult assemble(const Frame& frame,
                            std::vector<uint8_t>& access_unit,
                            uint16_t& marker_sequence,
                            uint32_t& marker_extended_sequence) {
        counters.assembly_attempts++;
        if (!frame.marker_seen) return AssembleResult::Incomplete;

        const Packet* marker = nullptr;
        const Packet* first_media = nullptr;
        for (const auto& packet : frame.packets) {
            if (packet.payload_size > 0 && !first_media) first_media = &packet;
            if (packet.marker && packet.payload_size > 0) marker = &packet;
        }
        if (!marker || !first_media) return AssembleResult::Incomplete;
        if (!isPartitionHead(packetPayload(frame, *first_media),
                             first_media->payload_size)) {
            return AssembleResult::Incomplete;
        }
        if (first_media->extended_sequence > marker->extended_sequence) {
            return AssembleResult::Invalid;
        }
        if (!sequenceRangeReceived(first_media->extended_sequence,
                                   marker->extended_sequence)) {
            return AssembleResult::Incomplete;
        }

        access_unit.clear();
        access_unit.reserve(std::min(kMaxAccessUnitBytes,
                                     frame.payload_storage.size() +
                                         frame.packets.size() * sizeof(kStartCode)));
        H264AssemblyState assembly_state;
        for (const auto& packet : frame.packets) {
            if (packet.extended_sequence < first_media->extended_sequence ||
                packet.extended_sequence > marker->extended_sequence ||
                packet.payload_size == 0) {
                continue;
            }
            switch (depacketize(packetPayload(frame, packet), packet.payload_size,
                                access_unit, assembly_state)) {
                case DepacketizeResult::Ok:
                    break;
                case DepacketizeResult::Unsupported:
                    counters.unsupported_nalus++;
                    return AssembleResult::Invalid;
                case DepacketizeResult::Invalid:
                    return AssembleResult::Invalid;
                case DepacketizeResult::Overflow:
                    return AssembleResult::Overflow;
            }
        }
        if (assembly_state.fu_active) return AssembleResult::Invalid;
        if (!waiting_keyframe && have_last_consumed_sequence &&
            first_media->extended_sequence > last_consumed_sequence + 1 &&
            !sequenceRangeReceived(last_consumed_sequence + 1,
                                   first_media->extended_sequence - 1) &&
            !containsIdr(access_unit)) {
            return AssembleResult::CompleteDiscontinuous;
        }
        marker_sequence = marker->sequence;
        marker_extended_sequence = marker->extended_sequence;
        return access_unit.empty() ? AssembleResult::Invalid
                                   : AssembleResult::Complete;
    }

    void drain(uint64_t now_ms,
               const EmitCallback& emit,
               const RecoveryCallback& recovery) {
        std::vector<uint8_t> access_unit;
        while (!frames.empty()) {
            Frame& front = frames.front();
            uint16_t marker_sequence = 0;
            uint32_t marker_extended_sequence = 0;
            const AssembleResult result = front.marker_seen
                ? assemble(front, access_unit, marker_sequence,
                           marker_extended_sequence)
                : AssembleResult::Incomplete;
            if (result == AssembleResult::Complete ||
                result == AssembleResult::CompleteDiscontinuous) {
                const uint32_t timestamp = front.timestamp;
                const bool idr = containsIdr(access_unit);
                last_timestamp = timestamp;
                have_last_timestamp = true;
                last_consumed_sequence = marker_extended_sequence;
                have_last_consumed_sequence = true;
                removeFrontFrame();

                if (waiting_keyframe && !idr) {
                    counters.resyncs++;
                    notifyRecovery(recovery, false, now_ms, false);
                    continue;
                }

                waiting_keyframe = false;
                if (idr) {
                    soft_recovery_active = false;
                    soft_losses_in_window = 0;
                    recovery_notified = false;
                    decoder_reset_notified = false;
                } else if (result == AssembleResult::CompleteDiscontinuous) {
                    requestSoftRecovery(recovery, now_ms);
                } else if (soft_recovery_active) {
                    notifyRecovery(recovery, false, now_ms, false);
                } else {
                    recovery_notified = false;
                }
                counters.frames++;
                counters.max_frame_bytes = std::max(
                    counters.max_frame_bytes,
                    static_cast<uint32_t>(access_unit.size()));
                if (emit) {
                    try {
                        emit(access_unit.data(), access_unit.size(),
                             marker_sequence, timestamp);
                    } catch (...) {
                    }
                }
                continue;
            }

            const uint64_t frame_age_ms = elapsedMs(now_ms, front.first_seen_ms);
            const uint64_t idle_age_ms = elapsedMs(now_ms, front.last_progress_ms);
            const bool contains_idr = frameMayContainIdr(front);
            const bool recovery_candidate = waiting_keyframe &&
                frameMayContainRecoveryPoint(front);
            const uint64_t frame_hold_ms = recovery_candidate
                ? recovery_hold_ms
                : hold_ms;
            const bool idle_timeout = idle_age_ms >= frame_hold_ms;
            const bool hard_timeout = frame_age_ms >= kMaxFrameHoldMs;
            if (result == AssembleResult::Invalid ||
                result == AssembleResult::Overflow || idle_timeout ||
                hard_timeout) {
                const uint32_t frame_timestamp = front.timestamp;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
                const bool marker_seen = front.marker_seen;
                const bool partition_head_seen = frameHasPartitionHead(front);
                const size_t frame_packets = front.packets.size();
#endif
                uint32_t last_sequence = 0;
                bool have_last_sequence = false;
                for (const auto& packet : front.packets) {
                    if (!have_last_sequence ||
                        packet.extended_sequence > last_sequence) {
                        last_sequence = packet.extended_sequence;
                        have_last_sequence = true;
                    }
                }
                last_timestamp = front.timestamp;
                have_last_timestamp = true;
                if (have_last_sequence) {
                    last_consumed_sequence = last_sequence;
                    have_last_consumed_sequence = true;
                }
                removeFrontFrame();
                counters.corrupt_frames++;
                if (result == AssembleResult::Overflow) counters.overflow_frames++;
                bool hard_recovery = result == AssembleResult::Invalid ||
                    result == AssembleResult::Overflow || hard_timeout ||
                    waiting_keyframe || contains_idr;
                if (!hard_recovery && idle_timeout) {
                    hard_recovery = softLossRequiresHardRecovery(now_ms);
                }
                const bool reset_decoder = result == AssembleResult::Invalid ||
                    result == AssembleResult::Overflow;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
                const char* reject_reason = result == AssembleResult::Invalid
                    ? "invalid_h264"
                    : result == AssembleResult::Overflow
                        ? "access_unit_overflow"
                        : hard_timeout ? "hard_timeout" : "idle_timeout";
                lunar::dropDiagnosticLog(
                    "rtp-jitter",
                    "reject_reason=%s hard_recovery=%d reset_decoder=%d "
                    "frame_age_ms=%llu "
                    "idle_age_ms=%llu hold_ms=%llu packets=%zu marker_seen=%d "
                    "partition_head_seen=%d contains_idr=%d",
                    reject_reason,
                    hard_recovery ? 1 : 0,
                    reset_decoder ? 1 : 0,
                    static_cast<unsigned long long>(frame_age_ms),
                    static_cast<unsigned long long>(idle_age_ms),
                    static_cast<unsigned long long>(frame_hold_ms),
                    frame_packets,
                    marker_seen ? 1 : 0,
                    partition_head_seen ? 1 : 0,
                    contains_idr ? 1 : 0);
#endif
                if (hard_recovery) {
                    enterRecovery(recovery, now_ms, frame_timestamp,
                                  reset_decoder,
                                  "incomplete frame");
                } else {
                    requestSoftRecovery(recovery, now_ms);
                }
                continue;
            }
            break;
        }
    }

    void overflow(const RecoveryCallback& recovery, uint64_t now_ms) {
        clearFrames();
        clearPendingNacks();
        counters.overflow_frames++;
        counters.corrupt_frames++;
        enterRecovery(recovery, now_ms, 0, true, "jitter buffer overflow");
    }

    void receive(const uint8_t* packet,
                 size_t size,
                 uint64_t now_ms,
                 const EmitCallback& emit,
                 const NackCallback& nack,
                 const RecoveryCallback& recovery) {
        ParsedRtp parsed;
        if (!parseRtp(packet, size, parsed)) return;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        if (counters.last_arrival_ms > 0 && now_ms >= counters.last_arrival_ms) {
            counters.last_arrival_gap_ms = now_ms - counters.last_arrival_ms;
            counters.max_arrival_gap_ms = std::max(
                counters.max_arrival_gap_ms,
                counters.last_arrival_gap_ms);
        }
        counters.last_arrival_ms = now_ms;
        if (!have_ssrc) {
            have_ssrc = true;
            counters.ssrc = parsed.ssrc;
        } else if (parsed.ssrc != counters.ssrc) {
            counters.ssrc = parsed.ssrc;
            counters.ssrc_changes++;
        }
#endif
        counters.packets++;

        drainPendingNacks(now_ms, nack);

        const bool recovery_authorized =
            packetMayContainIdr(parsed.payload, parsed.payload_size);
        const SequenceUpdate sequence = noteSequence(parsed.sequence,
                                                     parsed.timestamp,
                                                     now_ms,
                                                     nack,
                                                     recovery_authorized);
        if (sequence.duplicate) return;

        if (parsed.payload_size > kMaxRtpPayloadBytes) {
            overflow(recovery, now_ms);
            return;
        }

        Frame* frame = findFrame(parsed.timestamp);
        if (parsed.payload_size == 0 && !frame) {
            drain(now_ms, emit, recovery);
            return;
        }
        if (!frame && have_last_timestamp &&
            !timestampNewer(parsed.timestamp, last_timestamp)) {
            // A retransmission may arrive after its frame timed out. It still
            // repairs receiver-report accounting, but must not clear newer
            // buffered frames or be mistaken for a capacity overflow.
            drain(now_ms, emit, recovery);
            return;
        }
        if (!frame) {
            frame = createFrame(parsed.timestamp, now_ms);
            if (!frame) {
                overflow(recovery, now_ms);
                return;
            }
        }
        if (buffered_packets >= kMaxBufferedPackets ||
            parsed.payload_size > kMaxBufferedBytes - buffered_bytes) {
            overflow(recovery, now_ms);
            return;
        }

        Packet buffered;
        buffered.sequence = parsed.sequence;
        buffered.extended_sequence = sequence.extended_sequence;
        buffered.marker = parsed.marker;
        buffered.payload_offset = frame->payload_storage.size();
        buffered.payload_size = parsed.payload_size;
        const size_t previous_capacity = frame->payload_storage.capacity();
        frame->payload_storage.insert(frame->payload_storage.end(),
                                      parsed.payload,
                                      parsed.payload + parsed.payload_size);
        if (frame->payload_storage.capacity() != previous_capacity) {
            counters.payload_storage_reallocations++;
        }
        const auto position = std::lower_bound(
            frame->packets.begin(), frame->packets.end(),
            buffered.extended_sequence,
            [](const Packet& packet, uint32_t extended_sequence) {
                return packet.extended_sequence < extended_sequence;
            });
        frame->packets.insert(position, buffered);
        frame->marker_seen = frame->marker_seen ||
                             (parsed.marker && parsed.payload_size > 0);
        if (parsed.payload_size > 0) frame->last_progress_ms = now_ms;
        buffered_packets++;
        buffered_bytes += parsed.payload_size;
        if (waiting_keyframe && recovery_authorized) {
            authorizeRecoveryNacks(parsed.timestamp, now_ms, nack);
        }
        drain(now_ms, emit, recovery);
    }

    VideoRtpJitterStats stats() const {
        VideoRtpJitterStats result = counters;
        result.buffered_bytes = buffered_bytes;
        result.buffered_packets = buffered_packets;
        result.buffered_frames = frames.size();
        result.highest_sequence = highest_sequence;
        return result;
    }

    VideoRtpReceiverReport receiverReport() {
        VideoRtpReceiverReport report;
        if (!have_sequence) return report;
        const uint32_t expected = highest_sequence - base_sequence + 1;
        const uint32_t expected_interval = expected - expected_prior;
        const uint32_t received_interval = unique_received - received_prior;
        const uint32_t lost_interval = expected_interval > received_interval
            ? expected_interval - received_interval
            : 0;
        expected_prior = expected;
        received_prior = unique_received;
        report.fraction_lost = expected_interval == 0
            ? 0
            : static_cast<uint8_t>(std::min<uint32_t>(
                  255, (lost_interval << 8) / expected_interval));
        report.cumulative_lost = counters.missing_packets > 0x00ffffffu
            ? 0x00ffffffu
            : counters.missing_packets;
        report.highest_sequence = highest_sequence;
        return report;
    }
};

VideoRtpJitterBuffer::VideoRtpJitterBuffer()
    : impl_(std::make_unique<Impl>()) {}

VideoRtpJitterBuffer::~VideoRtpJitterBuffer() = default;

void VideoRtpJitterBuffer::reset() {
    impl_->reset();
}

void VideoRtpJitterBuffer::setHoldMs(uint64_t hold_ms) {
    impl_->hold_ms = std::max<uint64_t>(1, std::min<uint64_t>(hold_ms, 1000));
}

void VideoRtpJitterBuffer::setRecoveryHoldMs(uint64_t hold_ms) {
    impl_->recovery_hold_ms = std::max<uint64_t>(kDefaultHoldMs,
        std::min<uint64_t>(hold_ms, kMaxRecoveryHoldMs));
}

void VideoRtpJitterBuffer::receive(const uint8_t* packet,
                                   size_t size,
                                   uint64_t now_ms,
                                   const EmitCallback& emit,
                                   const NackCallback& nack,
                                   const RecoveryCallback& recovery) {
    try {
        impl_->receive(packet, size, now_ms, emit, nack, recovery);
    } catch (const std::bad_alloc&) {
        impl_->overflow(recovery, now_ms);
    } catch (...) {
        impl_->overflow(recovery, now_ms);
    }
}

VideoRtpJitterStats VideoRtpJitterBuffer::stats() const {
    return impl_->stats();
}

VideoRtpReceiverReport VideoRtpJitterBuffer::receiverReport() {
    return impl_->receiverReport();
}

bool VideoRtpJitterBuffer::waitingForKeyframe() const {
    return impl_->waiting_keyframe;
}

} // namespace lunar::webrtc
