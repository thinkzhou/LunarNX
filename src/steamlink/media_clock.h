#pragma once
#include <cstdint>
#include <mutex>
namespace lunar::steamlink {
class MediaClock {
public:
    void reset() { std::lock_guard<std::mutex> lock(mutex_); initialized_ = false; }
    uint64_t map(uint32_t timestamp) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            initialized_ = true;
            last_ = timestamp;
            ticks_ = 65536; // one-second headroom for the other track arriving earlier
        } else {
            const uint32_t delta = timestamp - last_;
            ticks_ += delta <= INT32_MAX ? int64_t(delta) : int64_t(delta) - (int64_t(1) << 32);
            last_ = timestamp;
        }
        return ticks_ > 0 ? uint64_t(ticks_) * 1000000000ULL / 65536 : 0;
    }
private:
    std::mutex mutex_;
    bool initialized_ = false;
    uint32_t last_ = 0;
    int64_t ticks_ = 0;
};
}
