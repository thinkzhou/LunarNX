#pragma once

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <sys/stat.h>

#ifndef LUNARNX_DIAGNOSTIC_LOG
#define LUNARNX_DIAGNOSTIC_LOG 1
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

// Never throw: stream threads call this heavily. A failed mutex/file op must not
// tear down the stream loop.
inline void diagnosticLog(const char* component, const char* format, ...) noexcept {
#if LUNARNX_DIAGNOSTIC_LOG
    try {
        ensureDiagnosticLogDirectory();

        static std::mutex log_mutex;
        std::lock_guard<std::mutex> lock(log_mutex);

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

} // namespace lunar
