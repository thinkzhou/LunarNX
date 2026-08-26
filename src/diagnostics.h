#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/stat.h>

#ifndef LUNARNX_DIAGNOSTIC_LOG
#define LUNARNX_DIAGNOSTIC_LOG 0
#endif

#ifndef LUNARNX_DROP_DIAGNOSTIC_LOG
#define LUNARNX_DROP_DIAGNOSTIC_LOG 0
#endif

#ifndef LUNARNX_LATENCY_DIAGNOSTIC_LOG
#define LUNARNX_LATENCY_DIAGNOSTIC_LOG 0
#endif

namespace lunar {

inline const char* get_diagnostic_log_path() {
#ifdef __SWITCH__
    return "sdmc:/switch/LunarNX/lunarnx.log";
#else
    return "./lunarnx.log";
#endif
}

inline void ensureDiagnosticLogDirectory() {
#ifdef __SWITCH__
    ::mkdir("sdmc:/switch", 0777);
    ::mkdir("sdmc:/switch/LunarNX", 0777);
#endif
}

inline std::mutex& diagnosticLogMutex() {
    static std::mutex mutex;
    return mutex;
}

inline uint64_t diagnosticMonotonicMs() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
}

// UTC wall-clock timestamps make appended logs unambiguous across process
// restarts and avoid depending on the console/emulator timezone configuration.
inline void diagnosticTimestamp(char* buffer, size_t buffer_size) noexcept {
    if (!buffer || buffer_size == 0) return;

    const auto now = std::chrono::system_clock::now();
    const auto since_epoch = now.time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        since_epoch).count() % 1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_time{};
#ifdef _WIN32
    const bool converted = gmtime_s(&utc_time, &time) == 0;
#else
    const bool converted = gmtime_r(&time, &utc_time) != nullptr;
#endif
    if (!converted) {
        std::snprintf(buffer, buffer_size, "0000-00-00T00:00:00.000Z");
        return;
    }
    std::snprintf(buffer, buffer_size,
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  utc_time.tm_year + 1900, utc_time.tm_mon + 1,
                  utc_time.tm_mday, utc_time.tm_hour, utc_time.tm_min,
                  utc_time.tm_sec, static_cast<long long>(milliseconds));
}

#if LUNARNX_DROP_DIAGNOSTIC_LOG || LUNARNX_LATENCY_DIAGNOSTIC_LOG
inline constexpr size_t kDropDiagnosticQueueCapacity = 64;
inline constexpr size_t kDropDiagnosticLineBytes = 4096;

struct AsyncDiagnosticWriterStats {
    uint64_t enqueued = 0;
    uint64_t dropped = 0;
    uint64_t batches = 0;
    uint64_t bytes_written = 0;
    uint64_t write_total_us = 0;
    uint64_t write_max_us = 0;
    uint64_t file_opens = 0;
    uint64_t flushes = 0;
    uint32_t queue_depth = 0;
    uint32_t queue_high_watermark = 0;
};

namespace detail {

struct DropDiagnosticEntry {
    std::array<char, kDropDiagnosticLineBytes> text{};
    size_t length = 0;
    bool urgent = false;
};

class DropDiagnosticWriter {
public:
    DropDiagnosticWriter() = default;

    ~DropDiagnosticWriter() {
        stop();
    }

    void start() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_) return;
            if (entries_.empty()) {
                entries_.resize(kDropDiagnosticQueueCapacity);
            }
            stopping_ = false;
            worker_ = std::thread([this]() { run(); });
            running_ = true;
        } catch (...) {
            running_ = false;
        }
    }

    void stop() noexcept {
        try {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!running_) return;
                stopping_ = true;
            }
            cv_.notify_one();
            if (worker_.joinable() &&
                worker_.get_id() != std::this_thread::get_id()) {
                worker_.join();
            }
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            stopping_ = false;
            head_ = 0;
            count_ = 0;
        } catch (...) {
            try {
                if (worker_.joinable()) worker_.detach();
            } catch (...) {
            }
        }
    }

    bool enqueue(const char* text, size_t length, bool urgent) noexcept {
        if (!text || length == 0) return false;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || stopping_ || count_ >= entries_.size()) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            const size_t index = (head_ + count_) % entries_.size();
            auto& entry = entries_[index];
            entry.length = std::min(length, entry.text.size() - 1);
            std::memcpy(entry.text.data(), text, entry.length);
            entry.text[entry.length] = '\0';
            entry.urgent = urgent;
            count_++;
            enqueued_.fetch_add(1, std::memory_order_relaxed);
            uint32_t high = queue_high_watermark_.load(
                std::memory_order_relaxed);
            const uint32_t depth = static_cast<uint32_t>(count_);
            while (depth > high &&
                   !queue_high_watermark_.compare_exchange_weak(
                       high, depth, std::memory_order_relaxed)) {}
            cv_.notify_one();
            return true;
        } catch (...) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    bool running() const noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return running_ && !stopping_;
        } catch (...) {
            return false;
        }
    }

    AsyncDiagnosticWriterStats stats() const noexcept {
        AsyncDiagnosticWriterStats result;
        result.enqueued = enqueued_.load(std::memory_order_relaxed);
        result.dropped = dropped_.load(std::memory_order_relaxed);
        result.batches = batches_.load(std::memory_order_relaxed);
        result.bytes_written = bytes_written_.load(std::memory_order_relaxed);
        result.write_total_us = write_total_us_.load(std::memory_order_relaxed);
        result.write_max_us = write_max_us_.load(std::memory_order_relaxed);
        result.file_opens = file_opens_.load(std::memory_order_relaxed);
        result.flushes = flushes_.load(std::memory_order_relaxed);
        result.queue_high_watermark = queue_high_watermark_.load(
            std::memory_order_relaxed);
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            result.queue_depth = static_cast<uint32_t>(count_);
        } catch (...) {
        }
        return result;
    }

private:
    bool hasUrgentLocked() const noexcept {
        for (size_t offset = 0; offset < count_; ++offset) {
            const size_t index = (head_ + offset) % entries_.size();
            if (entries_[index].urgent) return true;
        }
        return false;
    }

    void run() noexcept {
        std::vector<DropDiagnosticEntry> batch;
        try {
            batch.reserve(kDropDiagnosticQueueCapacity);
        } catch (...) {
            return;
        }

        FILE* log = nullptr;
        auto last_flush = std::chrono::steady_clock::now();
        while (true) {
            batch.clear();
            bool should_stop = false;
            bool batch_urgent = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stopping_ || count_ > 0; });
                if (!stopping_ && !hasUrgentLocked()) {
                    cv_.wait_for(lock, std::chrono::milliseconds(100),
                                 [this]() {
                                     return stopping_ || hasUrgentLocked();
                                 });
                }
                while (count_ > 0) {
                    batch_urgent = batch_urgent || entries_[head_].urgent;
                    batch.push_back(entries_[head_]);
                    head_ = (head_ + 1) % entries_.size();
                    count_--;
                }
                should_stop = stopping_;
            }

            try {
                const auto write_started = std::chrono::steady_clock::now();
                uint64_t batch_bytes = 0;
                ensureDiagnosticLogDirectory();
                std::lock_guard<std::mutex> file_lock(diagnosticLogMutex());
                if (!log) {
                    log = std::fopen(get_diagnostic_log_path(), "a");
                    if (log) {
                        std::setvbuf(log, nullptr, _IOFBF, 16 * 1024);
                        file_opens_.fetch_add(1, std::memory_order_relaxed);
                        last_flush = std::chrono::steady_clock::now();
                    }
                }
                if (log) {
                    const uint64_t dropped_now = dropped();
                    if (dropped_now != reported_drops_) {
                        std::fprintf(
                            log,
                            "[drop-diag t=%llums writer] diag_queue_dropped=%llu\n",
                            static_cast<unsigned long long>(diagnosticMonotonicMs()),
                            static_cast<unsigned long long>(dropped_now));
                        reported_drops_ = dropped_now;
                    }
                    for (const auto& entry : batch) {
                        std::fwrite(entry.text.data(), 1, entry.length, log);
                        batch_bytes += entry.length;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (batch_urgent || should_stop ||
                        now - last_flush >= std::chrono::seconds(1)) {
                        std::fflush(log);
                        flushes_.fetch_add(1, std::memory_order_relaxed);
                        last_flush = now;
                    }
                    const uint64_t write_us = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - write_started).count());
                    batches_.fetch_add(1, std::memory_order_relaxed);
                    bytes_written_.fetch_add(batch_bytes,
                                             std::memory_order_relaxed);
                    write_total_us_.fetch_add(write_us,
                                              std::memory_order_relaxed);
                    uint64_t high = write_max_us_.load(
                        std::memory_order_relaxed);
                    while (write_us > high &&
                           !write_max_us_.compare_exchange_weak(
                               high, write_us, std::memory_order_relaxed)) {}
                }
            } catch (...) {
            }

            if (should_stop) {
                try {
                    std::lock_guard<std::mutex> file_lock(
                        diagnosticLogMutex());
                    if (log) {
                        std::fclose(log);
                        log = nullptr;
                    }
                } catch (...) {
                }
                return;
            }
        }
    }

    std::vector<DropDiagnosticEntry> entries_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    size_t head_ = 0;
    size_t count_ = 0;
    bool running_ = false;
    bool stopping_ = false;
    std::atomic<uint64_t> enqueued_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> batches_{0};
    std::atomic<uint64_t> bytes_written_{0};
    std::atomic<uint64_t> write_total_us_{0};
    std::atomic<uint64_t> write_max_us_{0};
    std::atomic<uint64_t> file_opens_{0};
    std::atomic<uint64_t> flushes_{0};
    std::atomic<uint32_t> queue_high_watermark_{0};
    uint64_t reported_drops_ = 0;
};

inline DropDiagnosticWriter& dropDiagnosticWriter() {
    static DropDiagnosticWriter writer;
    return writer;
}

} // namespace detail

inline void startDropDiagnosticWriter() noexcept {
    detail::dropDiagnosticWriter().start();
}

inline void stopDropDiagnosticWriter() noexcept {
    detail::dropDiagnosticWriter().stop();
}

inline bool enqueueDropDiagnostic(const char* text,
                                  size_t length,
                                  bool urgent = false) noexcept {
    return detail::dropDiagnosticWriter().enqueue(text, length, urgent);
}

inline bool dropDiagnosticWriterRunning() noexcept {
    return detail::dropDiagnosticWriter().running();
}

inline uint64_t dropDiagnosticQueueDrops() noexcept {
    return detail::dropDiagnosticWriter().dropped();
}

inline AsyncDiagnosticWriterStats asyncDiagnosticWriterStats() noexcept {
    return detail::dropDiagnosticWriter().stats();
}
#else
struct AsyncDiagnosticWriterStats {
    uint64_t enqueued = 0;
    uint64_t dropped = 0;
    uint64_t batches = 0;
    uint64_t bytes_written = 0;
    uint64_t write_total_us = 0;
    uint64_t write_max_us = 0;
    uint64_t file_opens = 0;
    uint64_t flushes = 0;
    uint32_t queue_depth = 0;
    uint32_t queue_high_watermark = 0;
};
inline void startDropDiagnosticWriter() noexcept {}
inline void stopDropDiagnosticWriter() noexcept {}
inline bool dropDiagnosticWriterRunning() noexcept { return false; }
inline uint64_t dropDiagnosticQueueDrops() noexcept { return 0; }
inline AsyncDiagnosticWriterStats asyncDiagnosticWriterStats() noexcept {
    return {};
}
#endif

inline std::atomic<bool>& cloud1080CrashProbeFlag() {
    static std::atomic<bool> enabled{false};
    return enabled;
}

inline void setCloud1080CrashProbeEnabled(bool enabled) noexcept {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    cloud1080CrashProbeFlag().store(enabled, std::memory_order_release);
#else
    (void)enabled;
#endif
}

inline bool cloud1080CrashProbeEnabled() noexcept {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    return cloud1080CrashProbeFlag().load(std::memory_order_acquire);
#else
    return false;
#endif
}

inline bool shouldSampleCloud1080CrashProbe(uint64_t one_based_index) noexcept {
    return cloud1080CrashProbeEnabled() &&
           (one_based_index <= 4 || one_based_index == 8 ||
            one_based_index == 12 || one_based_index == 24 ||
            one_based_index == 60 || one_based_index == 120 ||
            one_based_index == 240);
}

// Never throw: stream threads call this heavily. A failed mutex/file op must not
// tear down the stream loop.
inline void diagnosticLog(const char* component, const char* format, ...) noexcept {
#if LUNARNX_DIAGNOSTIC_LOG
    try {
        ensureDiagnosticLogDirectory();

        std::lock_guard<std::mutex> lock(diagnosticLogMutex());

        FILE* log = std::fopen(get_diagnostic_log_path(), "a");
        if (!log) return;

        char timestamp[80]{};
        diagnosticTimestamp(timestamp, sizeof(timestamp));
        std::fprintf(log, "[%s] [%s] ", timestamp, component ? component : "diag");

        va_list args;
        va_start(args, format);
        std::vfprintf(log, format, args);
        va_end(args);

        std::fprintf(log, "\n");
        std::fclose(log);
    } catch (...) {
        // Swallow all logging failures.
    }
#else
    (void) component;
    (void) format;
#endif
}

// Rare, user-visible failures must remain diagnosable in release builds where
// APP_DIAG=0. Prefer sanitized metadata. Raw protocol traces are reserved for
// bounded, user-initiated troubleshooting flows and must never be per-packet.
inline void persistentEventLog(const char* component, const char* format, ...) noexcept {
    try {
        va_list args;
        va_start(args, format);
#if LUNARNX_DROP_DIAGNOSTIC_LOG || LUNARNX_LATENCY_DIAGNOSTIC_LOG
        if (dropDiagnosticWriterRunning()) {
            std::array<char, kDropDiagnosticLineBytes> line{};
            char timestamp[80]{};
            diagnosticTimestamp(timestamp, sizeof(timestamp));
            int written = std::snprintf(
                line.data(), line.size(), "[%s] [%s] ", timestamp,
                component ? component : "event");
            if (written >= 0) {
                size_t used = std::min(static_cast<size_t>(written),
                                       line.size() - 1);
                va_list args_copy;
                va_copy(args_copy, args);
                const int body_written = std::vsnprintf(
                    line.data() + used, line.size() - used, format,
                    args_copy);
                va_end(args_copy);
                if (body_written > 0) {
                    used += std::min(static_cast<size_t>(body_written),
                                     line.size() - used - 1);
                }
                if (used + 1 < line.size()) {
                    line[used++] = '\n';
                    line[used] = '\0';
                }
                if (enqueueDropDiagnostic(line.data(), used, true)) {
                    va_end(args);
                    return;
                }
            }
        }
#endif
        ensureDiagnosticLogDirectory();
        std::lock_guard<std::mutex> lock(diagnosticLogMutex());
        FILE* log = std::fopen(get_diagnostic_log_path(), "a");
        if (!log) {
            va_end(args);
            return;
        }
        char timestamp[80]{};
        diagnosticTimestamp(timestamp, sizeof(timestamp));
        std::fprintf(log, "[%s] [%s] ", timestamp,
                     component ? component : "event");
        std::vfprintf(log, format, args);
        va_end(args);
        std::fprintf(log, "\n");
        std::fclose(log);
    } catch (...) {
        // Logging must never turn a recoverable failure into a crash.
    }
}

// Sparse, always-available diagnostics for events that already caused a
// visible stream defect. Unlike diagnosticLog(), this stays enabled when the
// release build uses APP_DIAG=0; callers must never invoke it per frame.
inline void enqueueFormattedDropDiagnostic(bool urgent,
                                           const char* component,
                                           const char* format,
                                           va_list args) noexcept {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    try {
        std::array<char, kDropDiagnosticLineBytes> line{};
        int written = std::snprintf(
            line.data(),
            line.size(),
            "[drop-diag t=%llums %s] ",
            static_cast<unsigned long long>(diagnosticMonotonicMs()),
            component ? component : "stream");
        if (written < 0) return;
        size_t used = std::min(static_cast<size_t>(written), line.size() - 1);

        va_list args_copy;
        va_copy(args_copy, args);
        const int body_written = std::vsnprintf(
            line.data() + used,
            line.size() - used,
            format,
            args_copy);
        va_end(args_copy);
        if (body_written > 0) {
            used += std::min(static_cast<size_t>(body_written),
                             line.size() - used - 1);
        }
        if (used + 1 < line.size()) {
            line[used++] = '\n';
            line[used] = '\0';
        }
        enqueueDropDiagnostic(line.data(), used, urgent);
    } catch (...) {
        // A diagnostic must never take down a media worker.
    }
#else
    (void)urgent;
    (void) component;
    (void) format;
    (void) args;
#endif
}

inline void dropDiagnosticLog(const char* component,
                              const char* format,
                              ...) noexcept {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    va_list args;
    va_start(args, format);
    enqueueFormattedDropDiagnostic(false, component, format, args);
    va_end(args);
#else
    (void)component;
    (void)format;
#endif
}

// Periodic latency summaries use the same bounded background writer as sparse
// drop diagnostics. Callers aggregate hot-path samples in memory and invoke
// this at most once per reporting window; no media/input callback writes to the
// SD card directly.
#if defined(__GNUC__) || defined(__clang__)
inline void latencyDiagnosticLog(const char* component,
                                 const char* format,
                                 ...) noexcept
    __attribute__((format(printf, 2, 3)));
#endif
inline void latencyDiagnosticLog(const char* component,
                                 const char* format,
                                 ...) noexcept {
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
    try {
        std::array<char, kDropDiagnosticLineBytes> line{};
        int written = std::snprintf(
            line.data(), line.size(),
            "[latency-diag t=%llums %s] ",
            static_cast<unsigned long long>(diagnosticMonotonicMs()),
            component ? component : "stream");
        if (written < 0) return;
        size_t used = std::min(static_cast<size_t>(written), line.size() - 1);
        va_list args;
        va_start(args, format);
        const int body_written = std::vsnprintf(
            line.data() + used, line.size() - used, format, args);
        va_end(args);
        if (body_written > 0) {
            used += std::min(static_cast<size_t>(body_written),
                             line.size() - used - 1);
        }
        if (used + 1 < line.size()) {
            line[used++] = '\n';
            line[used] = '\0';
        }
        enqueueDropDiagnostic(line.data(), used, false);
    } catch (...) {
    }
#else
    (void)component;
    (void)format;
#endif
}

// Temporary, bounded crash breadcrumbs for the cloud 1080p hardware failure.
// They still use the async writer, but wake it immediately so a service crash
// is less likely to erase the final completed stage.
inline void cloud1080CrashProbeLog(const char* component,
                                   const char* format,
                                   ...) noexcept {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    if (!cloud1080CrashProbeEnabled()) return;
    va_list args;
    va_start(args, format);
    enqueueFormattedDropDiagnostic(true, component, format, args);
    va_end(args);
#else
    (void)component;
    (void)format;
#endif
}

} // namespace lunar
