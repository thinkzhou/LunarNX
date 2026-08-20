#include "../src/stream/bounded_video_queue_policy.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using namespace lunar::stream;

int main() {
    const auto decide = [](BoundedVideoQueueSnapshot queue, size_t bytes,
                           bool random_access) {
        return evaluateBoundedVideoAdmission(
            queue, bytes, random_access, 3, 8 * 1024 * 1024, 50ms);
    };
    assert(decide({0, 0, 0ms, false}, 1024, false) == BoundedVideoAdmission::Accept);
    assert(decide({3, 3072, 1ms, false}, 1024, false) == BoundedVideoAdmission::RecoverOverflow);
    assert(decide({1, 1024, 49ms, false}, 1024, false) == BoundedVideoAdmission::Accept);
    assert(decide({1, 1024, 50ms, false}, 1024, false) == BoundedVideoAdmission::RecoverAge);
    assert(decide({0, 0, 0ms, false}, 8 * 1024 * 1024 + 1, true) == BoundedVideoAdmission::RejectOversize);
    assert(decide({0, 0, 0ms, true}, 1024, false) == BoundedVideoAdmission::DropDependent);
    assert(decide({0, 0, 0ms, true}, 1024, true) == BoundedVideoAdmission::Accept);
    assert(boundedVideoResetMustPrecedeDecode(true, false, true));
    assert(!boundedVideoResetMustPrecedeDecode(false, false, true));
    assert(!boundedVideoMayDecodeWhileRecovering(true, false));
    assert(boundedVideoMayDecodeWhileRecovering(true, true));
    assert(boundedVideoPacketIsCurrent(true, 7, 7, 4, 4));
    assert(!boundedVideoPacketIsCurrent(true, 6, 7, 4, 4));
    assert(!boundedVideoPacketIsCurrent(true, 7, 7, 3, 4));
    assert(!boundedVideoPacketIsCurrent(false, 7, 7, 4, 4));

    std::atomic<bool> running{true};
    std::atomic<uint32_t> generation{9};
    std::atomic<bool> stale_was_accepted{false};
    std::thread shutdown_race([&]() {
        while (running.load()) {
            if (boundedVideoPacketIsCurrent(
                    running.load(), 8, generation.load(), 2, 2)) {
                stale_was_accepted = true;
            }
        }
    });
    generation = 10;
    running = false;
    shutdown_race.join();
    assert(!stale_was_accepted.load());
}
