#pragma once

#include <atomic>
#include <cstdint>

namespace lunar::common {

// A cheap cancellation epoch for detached work. A worker captures a Ticket
// before it starts and must discard results once the owner invalidates the
// generation. Tickets remain safe to inspect after the UI object is gone when
// the OperationGeneration itself is shared.
class OperationGeneration {
public:
    using Ticket = uint64_t;

    Ticket capture() const noexcept {
        return value_.load(std::memory_order_acquire);
    }

    bool isCurrent(Ticket ticket) const noexcept {
        return value_.load(std::memory_order_acquire) == ticket;
    }

    Ticket invalidate() noexcept {
        return value_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

private:
    std::atomic<Ticket> value_{1};
};

} // namespace lunar::common
