#pragma once

#ifdef __SWITCH__

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace lunar::ps {

// A launch-scoped, bounded trace for the PS connection path. record() only
// formats into memory: it never opens the SD log and is therefore safe to use
// between time-sensitive PSN/Chiaki stages. finish() writes the completed trace
// after failure/cancellation, or through the async writer after first video.
class PsConnectionTrace {
public:
    static constexpr size_t kMaxEntries = 80;

    PsConnectionTrace(const char* route, const char* platform) noexcept;

    PsConnectionTrace(const PsConnectionTrace&) = delete;
    PsConnectionTrace& operator=(const PsConnectionTrace&) = delete;

#if defined(__GNUC__) || defined(__clang__)
    void record(const char* stage, const char* outcome,
                const char* format, ...) noexcept
        __attribute__((format(printf, 4, 5)));
#else
    void record(const char* stage, const char* outcome,
                const char* format, ...) noexcept;
#endif

    void finish(const char* outcome, const char* detail) noexcept;
    bool finished() const noexcept;
    uint32_t id() const noexcept { return id_; }

private:
    struct Entry {
        uint64_t elapsed_ms = 0;
        std::array<char, 32> stage{};
        std::array<char, 16> outcome{};
        std::array<char, 224> detail{};
    };

    uint64_t elapsedMs() const noexcept;
    void appendLocked(const char* stage, const char* outcome,
                      const char* detail) noexcept;

    uint32_t id_ = 0;
    std::chrono::steady_clock::time_point started_;
    mutable std::mutex mutex_;
    std::array<Entry, kMaxEntries> entries_{};
    size_t count_ = 0;
    uint32_t dropped_ = 0;
    bool finished_ = false;
};

} // namespace lunar::ps

#endif
