#ifdef __SWITCH__

#include "ps_connection_trace.h"
#include "../diagnostics.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace lunar::ps {

namespace {

std::atomic<uint32_t> g_next_ps_connection_trace_id{1};

template <size_t N>
void copyText(std::array<char, N>& target, const char* value) noexcept {
    if (!value) value = "";
    std::snprintf(target.data(), target.size(), "%s", value);
}

} // namespace

PsConnectionTrace::PsConnectionTrace(const char* route,
                                     const char* platform) noexcept
    : id_(g_next_ps_connection_trace_id.fetch_add(
          1, std::memory_order_relaxed)),
      started_(std::chrono::steady_clock::now()) {
    char detail[96]{};
    std::snprintf(detail, sizeof(detail), "route=%s platform=%s",
                  route ? route : "unknown",
                  platform ? platform : "unknown");
    std::lock_guard<std::mutex> lock(mutex_);
    appendLocked("launch", "begin", detail);
}

uint64_t PsConnectionTrace::elapsedMs() const noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count());
}

void PsConnectionTrace::appendLocked(const char* stage, const char* outcome,
                                     const char* detail) noexcept {
    // Reserve the final slot for the terminal outcome so a noisy dependency
    // cannot hide whether the launch failed, was cancelled, or reached video.
    if (count_ >= kMaxEntries - 1) {
        dropped_++;
        return;
    }
    Entry& entry = entries_[count_++];
    entry.elapsed_ms = elapsedMs();
    copyText(entry.stage, stage ? stage : "unknown");
    copyText(entry.outcome, outcome ? outcome : "info");
    copyText(entry.detail, detail ? detail : "");
}

void PsConnectionTrace::record(const char* stage, const char* outcome,
                               const char* format, ...) noexcept {
    try {
        char detail[224]{};
        if (format && *format) {
            va_list args;
            va_start(args, format);
            std::vsnprintf(detail, sizeof(detail), format, args);
            va_end(args);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (finished_) return;
        appendLocked(stage, outcome, detail);
    } catch (...) {
        // Diagnostics must not turn a recoverable connection error into a crash.
    }
}

void PsConnectionTrace::finish(const char* outcome, const char* detail) noexcept {
    try {
        size_t count = 0;
        uint32_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (finished_) return;
            Entry& terminal = entries_[count_++];
            terminal.elapsed_ms = elapsedMs();
            copyText(terminal.stage, "launch");
            copyText(terminal.outcome, outcome ? outcome : "finished");
            copyText(terminal.detail, detail ? detail : "");
            finished_ = true;
            count = count_;
            dropped = dropped_;
        }

        // Failure/cancellation is already outside the timing-sensitive stage.
        // On success the caller starts the bounded async writer before finish().
        // Batch multiple entries per write so a long retry trace neither opens
        // the SD file dozens of times nor fills the async queue in one burst.
        constexpr size_t kEntriesPerBatch = 8;
        for (size_t begin = 0; begin < count; begin += kEntriesPerBatch) {
            std::array<char, 3584> batch{};
            size_t used = 0;
            const size_t end = std::min(count, begin + kEntriesPerBatch);
            for (size_t i = begin; i < end && used + 1 < batch.size(); ++i) {
                const Entry& entry = entries_[i];
                const int written = std::snprintf(
                    batch.data() + used, batch.size() - used,
                    "%strace=%u elapsed_ms=%llu stage=%s outcome=%s detail=%s",
                    i == begin ? "" : "\n",
                    id_, static_cast<unsigned long long>(entry.elapsed_ms),
                    entry.stage.data(), entry.outcome.data(),
                    entry.detail.data());
                if (written < 0) break;
                used += std::min(static_cast<size_t>(written),
                                 batch.size() - used - 1);
            }
            lunar::persistentEventLog("ps-connect", "%s", batch.data());
        }
        if (dropped > 0) {
            lunar::persistentEventLog(
                "ps-connect",
                "trace=%u elapsed_ms=%llu stage=trace outcome=truncated detail=dropped_entries=%u",
                id_, static_cast<unsigned long long>(elapsedMs()), dropped);
        }
    } catch (...) {
    }
}

bool PsConnectionTrace::finished() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return finished_;
    } catch (...) {
        return true;
    }
}

} // namespace lunar::ps

#endif
