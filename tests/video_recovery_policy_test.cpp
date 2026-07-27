#include "app/video_recovery_policy.h"

#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono_literals;
    lunar::app::VideoRecoveryTransportRetry retry;

    const auto start = std::chrono::steady_clock::time_point{};
    assert(retry.shouldRetry(true, start));
    assert(!retry.shouldRetry(true, start + 199ms));
    assert(retry.shouldRetry(true, start + 200ms));
    assert(!retry.shouldRetry(false, start + 1000ms));
    assert(retry.shouldRetry(true, start + 1001ms));
    return 0;
}
