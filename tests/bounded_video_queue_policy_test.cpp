#include "../src/stream/bounded_video_queue_policy.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using namespace lunar::stream;

int main() {
    const auto decide = [](BoundedVideoQueueSnapshot queue, size_t bytes,
                           bool random_access, bool has_vcl) {
        return evaluateBoundedVideoAdmission(
            queue, bytes, random_access, has_vcl, 3, 8 * 1024 * 1024, 50ms);
    };
    assert(decide({0, 0, 0ms, false}, 1024, false, true) == BoundedVideoAdmission::Accept);
    assert(decide({3, 3072, 1ms, false}, 1024, false, true) == BoundedVideoAdmission::RecoverOverflow);
    assert(decide({1, 1024, 49ms, false}, 1024, false, true) == BoundedVideoAdmission::Accept);
    assert(decide({1, 1024, 50ms, false}, 1024, false, true) == BoundedVideoAdmission::RecoverAge);
    assert(decide({0, 0, 0ms, false}, 8 * 1024 * 1024 + 1, true, true) == BoundedVideoAdmission::RejectOversize);
    assert(!boundedVideoAdmissionMayEnqueue(BoundedVideoAdmission::RejectOversize));
    assert(boundedVideoAdmissionMayEnqueue(BoundedVideoAdmission::Accept));
    assert(boundedVideoAdmissionMayEnqueue(BoundedVideoAdmission::RecoverOverflow));
    assert(decide({0, 0, 0ms, true}, 1024, false, true) == BoundedVideoAdmission::DropDependent);
    assert(decide({0, 0, 0ms, true}, 1024, true, true) == BoundedVideoAdmission::Accept);
    assert(decide({0, 0, 0ms, true}, 1024, false, false) == BoundedVideoAdmission::Accept);
    assert(boundedVideoResetMustPrecedeDecode(true, false, true));
    assert(!boundedVideoResetMustPrecedeDecode(false, false, true));
    assert(!boundedVideoMayDecodeWhileRecovering(true, false, true));
    assert(boundedVideoMayDecodeWhileRecovering(true, true, true));
    assert(boundedVideoMayDecodeWhileRecovering(true, false, false));

    BoundedVideoRecoveryState recovery;
    recovery.epoch = 12;
    recovery.reset_pending = false;
    recovery.waiting_for_keyframe = true;
    recovery.recovery_request = true;
    assert(!applyBoundedVideoRecovery(recovery, true, false, false));
    assert(recovery.epoch == 12);
    assert(!recovery.reset_pending);
    assert(recovery.waiting_for_keyframe);
    assert(recovery.recovery_request);
    assert(applyBoundedVideoRecovery(recovery, true, false, true));
    assert(recovery.epoch == 13);
    assert(recovery.reset_pending);
    assert(recovery.waiting_for_keyframe);
    assert(recovery.recovery_request);
    assert(!recovery.reset_wakeup);

    assert(boundedVideoPacketIsCurrent(true, 7, 7, 4, 4));
    assert(!boundedVideoPacketIsCurrent(true, 6, 7, 4, 4));
    assert(!boundedVideoPacketIsCurrent(true, 7, 7, 3, 4));
    assert(!boundedVideoPacketIsCurrent(false, 7, 7, 4, 4));
    assert(!realtimeVideoCapacityExceeded(2047, 1024, 1024, 2048, 32 * 1024 * 1024));
    assert(realtimeVideoCapacityExceeded(2048, 1024, 1024, 2048, 32 * 1024 * 1024));
    assert(realtimeVideoCapacityExceeded(1, 32 * 1024 * 1024, 1, 2048, 32 * 1024 * 1024));

    std::atomic<uint32_t> generation{8};
    std::atomic<bool> observed_current{false};
    std::atomic<bool> generation_changed{false};
    std::atomic<bool> stale_was_accepted{false};
    std::thread shutdown_race([&]() {
        while (!observed_current.load()) {
            if (boundedVideoPacketIsCurrent(true, 8, generation.load(), 2, 2)) {
                observed_current = true;
            }
        }
        while (!generation_changed.load()) {
            std::this_thread::yield();
        }
        stale_was_accepted = boundedVideoPacketIsCurrent(
            true, 8, generation.load(), 2, 2);
    });
    while (!observed_current.load()) std::this_thread::yield();
    generation = 9;
    generation_changed = true;
    shutdown_race.join();
    assert(observed_current.load());
    assert(!stale_was_accepted.load());
}
