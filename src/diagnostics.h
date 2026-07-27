#pragma once

#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <mutex>
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
        ensureDiagnosticLogDirectory();
        std::lock_guard<std::mutex> lock(diagnosticLogMutex());

        FILE* log = std::fopen(get_diagnostic_log_path(), "a");
        if (!log) return;

        std::fprintf(log, "[drop-diag t=%llums %s] ",
                     static_cast<unsigned long long>(diagnosticMonotonicMs()),
                     component ? component : "stream");

        va_list args;
        va_start(args, format);
        std::vfprintf(log, format, args);
        va_end(args);

        std::fprintf(log, "\n");
        std::fclose(log);
    } catch (...) {
        // A diagnostic must never take down a media worker.
    }
#else
    (void) component;
    (void) format;
#endif
}

} // namespace lunar
