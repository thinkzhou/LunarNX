#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace lunar::steamlink {

// Caller owns the lifecycle mutex. Detach before invoking protocol callbacks;
// repeated cleanup then cannot disconnect or free the same session twice.
template<class Session, class Disconnect, class Join, class Destroy>
void closeSession(Session*& slot, Disconnect disconnect, Join join, Destroy destroy) {
    auto* session = slot;
    slot = nullptr;
    if (!session) return;
    disconnect(session);
    join(session);
    destroy(session);
}

template<class Send>
bool sendKeyTransition(bool pressed, bool& previous, Send send) {
    if (pressed == previous) return false;
    if (!send(pressed)) return false;
    previous = pressed;
    return true;
}

class LogThrottle {
public:
    bool allow(uint64_t now_ms) {
        auto next = next_.load();
        return now_ms >= next && next_.compare_exchange_strong(next, now_ms + 1000);
    }
private:
    std::atomic<uint64_t> next_{0};
};

// Cancellation never touches a protocol object or waits for its owner lock.
class StreamCancellation {
public:
    void request() { canceled_.store(true); }
    bool requested() const { return canceled_.load(); }
    template<class Predicate>
    bool wait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
              std::chrono::milliseconds timeout, Predicate done) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done() && !requested()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            cv.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(10)));
        }
        return true;
    }
private:
    std::atomic<bool> canceled_{false};
};

class InputPump {
public:
    ~InputPump() { stop(); }
    void start(std::function<void()> update) {
        stop();
        running_ = true;
        thread_ = std::thread([this, update = std::move(update)] {
            while (running_.load()) {
                update();
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        });
    }
    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
private:
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// Convert AVCDecoderConfigurationRecord or Annex B parameter sets to Annex B.
// Empty data is valid: some hosts carry SPS/PPS in the keyframes instead.
inline bool h264Parameters(const uint8_t* data, size_t size,
                           std::vector<uint8_t>& result) {
    result.clear();
    if (!size) return true;
    if (!data || size > 65536) return false;
    if (size >= 4 && data[0] == 0 && data[1] == 0 &&
        (data[2] == 1 || (data[2] == 0 && data[3] == 1))) {
        result.assign(data, data + size);
        return true;
    }
    if (size < 7 || data[0] != 1) return false;
    size_t pos = 6;
    auto append = [&](unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            if (pos + 2 > size) return false;
            const size_t len = (size_t(data[pos]) << 8) | data[pos + 1];
            pos += 2;
            if (!len || len > size - pos) return false;
            result.insert(result.end(), {0, 0, 0, 1});
            result.insert(result.end(), data + pos, data + pos + len);
            pos += len;
        }
        return true;
    };
    if (!append(data[5] & 31) || pos >= size) { result.clear(); return false; }
    const unsigned pps = data[pos++];
    if (!append(pps)) { result.clear(); return false; }
    return !result.empty();
}

inline std::vector<uint8_t> h264AccessUnit(const std::vector<uint8_t>& parameters,
                                         const uint8_t* data, size_t size) {
    std::vector<uint8_t> result(parameters);
    result.insert(result.end(), data, data + size);
    return result;
}
} // namespace lunar::steamlink
