#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lunar::stream {

enum class BoundedVideoAdmission { Accept, DropDependent, RecoverOverflow, RecoverAge, RejectOversize };

struct BoundedVideoQueueSnapshot {
    size_t packets = 0;
    size_t bytes = 0;
    std::chrono::steady_clock::duration oldest_age{};
    bool waiting_for_keyframe = false;
};

inline BoundedVideoAdmission evaluateBoundedVideoAdmission(
    const BoundedVideoQueueSnapshot& queue, size_t incoming_bytes,
    bool random_access, size_t max_packets, size_t max_bytes,
    std::chrono::steady_clock::duration max_age) {
    if (incoming_bytes > max_bytes) return BoundedVideoAdmission::RejectOversize;
    if (queue.waiting_for_keyframe && !random_access)
        return BoundedVideoAdmission::DropDependent;
    if (queue.packets > 0 && queue.oldest_age >= max_age)
        return BoundedVideoAdmission::RecoverAge;
    if (queue.packets > 0 &&
        (queue.packets >= max_packets || incoming_bytes > max_bytes - queue.bytes))
        return BoundedVideoAdmission::RecoverOverflow;
    return BoundedVideoAdmission::Accept;
}

inline bool boundedVideoPacketIsCurrent(bool running, uint32_t packet_generation,
                                        uint32_t worker_generation,
                                        uint32_t packet_epoch,
                                        uint32_t recovery_epoch) {
    return running && packet_generation == worker_generation &&
           packet_epoch == recovery_epoch;
}

inline bool boundedVideoResetMustPrecedeDecode(bool reset_pending,
                                               bool reset_wakeup,
                                               bool random_access) {
    return reset_pending && (reset_wakeup || random_access);
}

inline bool boundedVideoMayDecodeWhileRecovering(bool waiting_for_keyframe,
                                                 bool random_access) {
    return !waiting_for_keyframe || random_access;
}

inline bool realtimeVideoCapacityExceeded(size_t packets, size_t bytes,
                                          size_t incoming_bytes,
                                          size_t max_packets,
                                          size_t max_bytes) {
    return packets > 0 &&
           (packets >= max_packets ||
            incoming_bytes > max_bytes - (bytes > max_bytes ? max_bytes : bytes));
}

inline bool boundedVideoAdmissionMayEnqueue(BoundedVideoAdmission admission) {
    return admission != BoundedVideoAdmission::DropDependent &&
           admission != BoundedVideoAdmission::RejectOversize;
}

} // namespace lunar::stream
