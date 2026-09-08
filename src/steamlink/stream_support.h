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

// Warnings only: a silent host must not terminate otherwise playable video.
class AudioProgressMonitor {
public:
    enum class Status { Healthy, Missing, Decode, Output, Unsupported };
    void reset() { initialized_ = false; }
    Status observe(uint64_t now, uint64_t received, uint64_t decoded,
                   uint64_t output, bool unsupported, bool suspended) {
        if (!initialized_ || suspended) {
            initialized_ = true;
            received_ = received; decoded_ = decoded; output_ = output;
            receive_at_ = decode_at_ = output_at_ = now;
            return Status::Healthy;
        }
        if (received_ != received) { received_ = received; receive_at_ = now; }
        if (decoded_ != decoded) { decoded_ = decoded; decode_at_ = now; }
        if (output_ != output) { output_ = output; output_at_ = now; }
        if (unsupported) return Status::Unsupported;
        auto expired = [now](uint64_t since) { return now >= since && now - since >= 20000000000ULL; };
        if (expired(receive_at_)) return Status::Missing;
        if (expired(decode_at_)) return Status::Decode;
        if (expired(output_at_)) return Status::Output;
        return Status::Healthy;
    }
private:
    bool initialized_ = false;
    uint64_t received_ = 0, decoded_ = 0, output_ = 0;
    uint64_t receive_at_ = 0, decode_at_ = 0, output_at_ = 0;
};

// Input-pump owned; audio traffic cannot reset any of these deadlines.
class VideoProgressWatchdog {
public:
    enum class Timeout { None, Receive, Decode, Present, Recovery };
    void reset() { initialized_ = false; recovering_ = false; }
    Timeout observe(uint64_t now, uint64_t received, uint64_t decoded,
                    uint64_t presented, bool recovery, bool suspended) {
        if (!initialized_ || suspended) {
            initialized_ = true;
            received_ = received; decoded_ = decoded; presented_ = presented;
            receive_at_ = decode_at_ = present_at_ = now;
            recovering_ = false;
            return Timeout::None;
        }
        if (received != received_) { received_ = received; receive_at_ = now; }
        if (decoded != decoded_) { decoded_ = decoded; decode_at_ = now; }
        if (presented != presented_) { presented_ = presented; present_at_ = now; }
        if (recovery && !recovering_) recovery_at_ = now;
        recovering_ = recovery;
        const auto expired = [now](uint64_t since) {
            return now >= since && now-since >= 20000000000ULL;
        };
        if (recovering_ && expired(recovery_at_)) return Timeout::Recovery;
        if (expired(receive_at_)) return Timeout::Receive;
        if (expired(decode_at_)) return Timeout::Decode;
        if (expired(present_at_)) return Timeout::Present;
        return Timeout::None;
    }
private:
    bool initialized_ = false, recovering_ = false;
    uint64_t received_=0, decoded_=0, presented_=0;
    uint64_t receive_at_=0, decode_at_=0, present_at_=0, recovery_at_=0;
};

class InputAvailabilityWatchdog {
public:
    void reset() { waiting_ = false; }
    bool expired(uint64_t now, bool opened) {
        if (opened) { waiting_ = false; return false; }
        if (!waiting_) { waiting_ = true; since_ = now; }
        return now >= since_ && now-since_ >= 20000000000ULL;
    }
private:
    bool waiting_ = false;
    uint64_t since_ = 0;
};

// Queue acceptance is not decoder success. An asynchronous recovery request
// must reach Steam even while the pipeline intentionally drops dependent frames.
class VideoRecoveryFeedback {
public:
    void reset() { next_ns_ = 0; }
    bool reportLost(bool queued, bool recovery_pending, uint64_t now_ns) {
        if (queued && !recovery_pending) return false;
        if (now_ns < next_ns_) return false;
        next_ns_ = now_ns + 1000000000ULL;
        return true;
    }
private:
    uint64_t next_ns_ = 0; // single video-submit worker
};

class MediaActivityWatchdog {
public:
    void reset(uint64_t now_ns) { activity_ = now_ns; }
    void received(uint64_t now_ns) {
        auto last = activity_.load();
        while (now_ns > last && !activity_.compare_exchange_weak(last, now_ns)) {}
    }
    bool expired(uint64_t now_ns) const {
        const auto last = activity_.load();
        return now_ns >= last && now_ns - last >= 20000000000ULL;
    }
private:
    std::atomic<uint64_t> activity_{0};
};

// Monotonic deadlines, independent of media packet arrival. Receiving audio or
// undecodable video must not keep the first rendered-frame wait alive forever.
class StartupWatchdog {
public:
    enum class Timeout { None, Connection, FirstFrame };
    void reset(uint64_t now_ns) { started_ = now_ns; connected_ = 0; }
    void connected(uint64_t now_ns) {
        uint64_t empty = 0;
        connected_.compare_exchange_strong(empty, now_ns);
    }
    Timeout expired(uint64_t now_ns) const {
        const auto connected = connected_.load();
        const auto since = connected ? connected : started_.load();
        const uint64_t limit = connected ? 20000000000ULL : 15000000000ULL;
        if (now_ns < since || now_ns - since < limit) return Timeout::None;
        return connected ? Timeout::FirstFrame : Timeout::Connection;
    }
private:
    std::atomic<uint64_t> started_{0}, connected_{0};
};

// Protocol disconnect initiation is separate from the user-visible Error state.
// A host disconnect and the owner cleanup can race without initiating twice.
class SessionDisconnectGate {
public:
    void reset() { initiated_ = false; }
    void disconnected() { initiated_ = true; }
    template<class Send> void request(Send send) {
        if (!initiated_.exchange(true)) send();
    }
private:
    std::atomic<bool> initiated_{false};
};

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
