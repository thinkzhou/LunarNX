#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lunar::stream {

enum class BoundedVideoAdmission { Accept, DropDependent, RecoverOverflow, RecoverAge, RejectOversize };

struct BoundedVideoRecoveryState {
    uint32_t epoch = 0;
    bool reset_pending = false;
    bool waiting_for_keyframe = false;
    bool recovery_request = false;
    bool reset_wakeup = false;
};

struct BoundedVideoQueueSnapshot {
    size_t packets = 0;
    size_t bytes = 0;
    std::chrono::steady_clock::duration oldest_age{};
    bool waiting_for_keyframe = false;
};

inline BoundedVideoAdmission evaluateBoundedVideoAdmission(
    const BoundedVideoQueueSnapshot& queue, size_t incoming_bytes,
    bool random_access, bool has_vcl, size_t max_packets, size_t max_bytes,
    std::chrono::steady_clock::duration max_age) {
    if (incoming_bytes > max_bytes) return BoundedVideoAdmission::RejectOversize;
    if (queue.waiting_for_keyframe && has_vcl && !random_access)
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
                                                 bool random_access,
                                                 bool has_vcl) {
    return !waiting_for_keyframe || !has_vcl || random_access;
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

inline bool applyBoundedVideoRecovery(BoundedVideoRecoveryState& state,
                                      bool running,
                                      bool worker_stop,
                                      bool force_new_epoch) {
    state.recovery_request = true;
    if (!running || worker_stop) return false;
    if (state.waiting_for_keyframe && !force_new_epoch) return false;

    ++state.epoch;
    state.reset_pending = true;
    state.waiting_for_keyframe = true;
    // Do not flush the decoder as soon as the queue trips the recovery
    // threshold. Keep the last rendered frame alive while waiting for the
    // recovery IDR; the video worker will reset immediately before decoding
    // that IDR.
    state.reset_wakeup = false;
    return true;
}

} // namespace lunar::stream
