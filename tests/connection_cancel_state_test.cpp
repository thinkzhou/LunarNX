#include "ui/connection_cancel_state.h"

#include <atomic>
#include <cassert>
#include <thread>
#include <vector>

using lunar::ui::ConnectionCancelState;

int main() {
    ConnectionCancelState failed_start;
    failed_start.markWorkerStarted();
    failed_start.markWorkerStartFailed();
    assert(!failed_start.workerStarted());
    assert(failed_start.workerDone());

    ConnectionCancelState state;
    assert(!state.cancelRequested());
    assert(!state.workerStarted());
    assert(!state.workerDone());
    assert(!state.cleanupClaimed());

    state.markWorkerStarted();
    assert(state.workerStarted());

    std::atomic<int> first_cancel_count{0};
    std::vector<std::thread> cancel_threads;
    for (int i = 0; i < 32; ++i) {
        cancel_threads.emplace_back([&]() {
            if (state.requestCancel()) ++first_cancel_count;
        });
    }
    for (auto& thread : cancel_threads) thread.join();
    assert(first_cancel_count.load() == 1);
    assert(state.cancelRequested());

    // Cancellation alone cannot claim teardown while the connection worker
    // may still be constructing the stream.
    assert(!state.tryClaimCleanup());

    state.markWorkerDone();
    assert(state.workerDone());

    std::atomic<int> cleanup_count{0};
    std::vector<std::thread> cleanup_threads;
    for (int i = 0; i < 32; ++i) {
        cleanup_threads.emplace_back([&]() {
            if (state.tryClaimCleanup()) ++cleanup_count;
        });
    }
    for (auto& thread : cleanup_threads) thread.join();
    assert(cleanup_count.load() == 1);
    assert(state.cleanupClaimed());

    // A failed worker launch may release the claim, but rapid repeated B
    // presses must still produce only one new owner.
    state.releaseCleanupClaim();
    cleanup_count = 0;
    cleanup_threads.clear();
    for (int i = 0; i < 32; ++i) {
        cleanup_threads.emplace_back([&]() {
            if (state.tryClaimCleanup()) ++cleanup_count;
        });
    }
    for (auto& thread : cleanup_threads) thread.join();
    assert(cleanup_count.load() == 1);

    return 0;
}
