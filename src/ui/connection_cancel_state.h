#pragma once

#include <atomic>

namespace lunar::ui {

// Coordinates a cancellable connection worker with exactly one teardown
// owner. All methods are lock-free because the UI and network workers only
// exchange lifecycle flags; the teardown itself remains externally serialized.
class ConnectionCancelState {
public:
    bool requestCancel() {
        return !cancel_requested_.exchange(true);
    }

    bool cancelRequested() const {
        return cancel_requested_.load();
    }

    void markWorkerStarted() {
        worker_started_ = true;
    }

    void markWorkerDone() {
        worker_done_ = true;
    }

    void markWorkerStartFailed() {
        worker_started_ = false;
        worker_done_ = true;
    }

    bool workerStarted() const {
        return worker_started_.load();
    }

    bool workerDone() const {
        return worker_done_.load();
    }

    bool tryClaimCleanup() {
        if (!cancelRequested() || !workerDone()) return false;
        return !cleanup_claimed_.exchange(true);
    }

    bool cleanupClaimed() const {
        return cleanup_claimed_.load();
    }

    void releaseCleanupClaim() {
        cleanup_claimed_ = false;
    }

private:
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> worker_started_{false};
    std::atomic<bool> worker_done_{false};
    std::atomic<bool> cleanup_claimed_{false};
};

} // namespace lunar::ui
