#include "common/operation_generation.h"

#include <atomic>
#include <cassert>
#include <thread>

using lunar::common::OperationGeneration;

int main() {
    OperationGeneration generation;
    const auto first = generation.capture();
    assert(generation.isCurrent(first));

    generation.invalidate();
    assert(!generation.isCurrent(first));
    const auto second = generation.capture();
    assert(generation.isCurrent(second));

    std::atomic<bool> started{false};
    std::atomic<bool> stale_response_applied{false};
    const auto request = generation.capture();
    std::thread delayed_response([&]() {
        started = true;
        while (generation.isCurrent(request)) {
            std::this_thread::yield();
        }
        stale_response_applied = generation.isCurrent(request);
    });
    while (!started.load()) std::this_thread::yield();
    generation.invalidate();
    delayed_response.join();
    assert(!stale_response_applied.load());

    return 0;
}
