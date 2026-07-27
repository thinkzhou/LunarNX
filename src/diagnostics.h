#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/stat.h>

#ifndef LUNARNX_DIAGNOSTIC_LOG
#define LUNARNX_DIAGNOSTIC_LOG 1
#endif

#ifndef LUNARNX_DROP_DIAGNOSTIC_LOG
#define LUNARNX_DROP_DIAGNOSTIC_LOG 1
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

#if LUNARNX_DROP_DIAGNOSTIC_LOG
inline constexpr size_t kDropDiagnosticQueueCapacity = 64;
inline constexpr size_t kDropDiagnosticLineBytes = 4096;

namespace detail {

struct DropDiagnosticEntry {
    std::array<char, kDropDiagnosticLineBytes> text{};
    size_t length = 0;
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

    bool enqueue(const char* text, size_t length) noexcept {
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
            count_++;
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

private:
    void run() noexcept {
        std::vector<DropDiagnosticEntry> batch;
        try {
            batch.reserve(kDropDiagnosticQueueCapacity);
        } catch (...) {
            return;
        }

        while (true) {
            batch.clear();
            bool should_stop = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stopping_ || count_ > 0; });
                if (!stopping_) {
                    cv_.wait_for(lock, std::chrono::milliseconds(20),
                                 [this]() { return stopping_; });
                }
                while (count_ > 0) {
                    batch.push_back(entries_[head_]);
                    head_ = (head_ + 1) % entries_.size();
                    count_--;
                }
                should_stop = stopping_;
            }

            try {
                ensureDiagnosticLogDirectory();
                std::lock_guard<std::mutex> file_lock(diagnosticLogMutex());
                FILE* log = std::fopen(get_diagnostic_log_path(), "a");
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
                    }
                    std::fclose(log);
                }
            } catch (...) {
            }

            if (should_stop) return;
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
    std::atomic<uint64_t> dropped_{0};
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

inline bool enqueueDropDiagnostic(const char* text, size_t length) noexcept {
    return detail::dropDiagnosticWriter().enqueue(text, length);
}

inline uint64_t dropDiagnosticQueueDrops() noexcept {
    return detail::dropDiagnosticWriter().dropped();
}
#else
inline void startDropDiagnosticWriter() noexcept {}
inline void stopDropDiagnosticWriter() noexcept {}
inline uint64_t dropDiagnosticQueueDrops() noexcept { return 0; }
#endif

// Never throw: stream threads call this heavily. A failed mutex/file op must not
// tear down the stream loop.
inline void diagnosticLog(const char* component, const char* format, ...) noexcept {
#if LUNARNX_DIAGNOSTIC_LOG
    try {
        ensureDiagnosticLogDirectory();

        std::lock_guard<std::mutex> lock(diagnosticLogMutex());

        FILE* log = std::fopen(get_diagnostic_log_path(), "a");
        if (!log) return;

        std::fprintf(log, "[%s] ", component ? component : "diag");

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

// Sparse, always-available diagnostics for events that already caused a
// visible stream defect. Unlike diagnosticLog(), this stays enabled when the
// release build uses APP_DIAG=0; callers must never invoke it per frame.
inline void dropDiagnosticLog(const char* component,
                              const char* format,
                              ...) noexcept {
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

        va_list args;
        va_start(args, format);
        const int body_written = std::vsnprintf(
            line.data() + used,
            line.size() - used,
            format,
            args);
        va_end(args);
        if (body_written > 0) {
            used += std::min(static_cast<size_t>(body_written),
                             line.size() - used - 1);
        }
        if (used + 1 < line.size()) {
            line[used++] = '\n';
            line[used] = '\0';
        }
        enqueueDropDiagnostic(line.data(), used);
    } catch (...) {
        // A diagnostic must never take down a media worker.
    }
#else
    (void) component;
    (void) format;
#endif
}

} // namespace lunar
