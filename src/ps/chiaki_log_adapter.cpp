#ifdef __SWITCH__

#include "chiaki_log_adapter.h"
#include "../diagnostics.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace lunar::ps {

enum class NoisyLogKind {
    None,
    Hexdump,
    Retransmit,
    AvLoss,
    DataDrop,
    SendBufferOverflow,
};

std::atomic<uint64_t> g_send_buffer_overflow_count{0};

static bool isPowerOfTwo(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static NoisyLogKind noisyLogKind(const char* msg) {
    if (!msg) return NoisyLogKind::None;
    if (std::strstr(msg, "offset 0") ||
        std::strstr(msg, "0123456789abcdef") ||
        std::strstr(msg, "Sending Message:") ||
        std::strstr(msg, "Receiving message:") ||
        std::strncmp(msg, "   ", 3) == 0) {
        return NoisyLogKind::Hexdump;
    }
    if (std::strstr(msg, "re-sending packet") ||
        std::strstr(msg, "Pushed seq num") ||
        std::strstr(msg, "Acked seq num") ||
        std::strstr(msg, "received data ack")) {
        return NoisyLogKind::Retransmit;
    }
    if (std::strstr(msg, "AV reorder timeout") ||
        std::strstr(msg, "Missing unit") ||
        std::strstr(msg, "FEC failed") ||
        std::strstr(msg, "corrupt frame") ||
        std::strstr(msg, "could not flush frame") ||
        std::strstr(msg, "Missing reference frame") ||
        std::strstr(msg, "Clamping reported packet loss") ||
        std::strstr(msg, "Congestion Control Packet")) {
        return NoisyLogKind::AvLoss;
    }
    if (std::strstr(msg, "Takion dropping data")) {
        return NoisyLogKind::DataDrop;
    }
    if (std::strstr(msg, "Takion Send Buffer overflow") ||
        std::strstr(msg, "Rudp Send Buffer overflow")) {
        return NoisyLogKind::SendBufferOverflow;
    }
    return NoisyLogKind::None;
}

static void writeLogLine(FILE* f, const char* timestamp,
                         const char* name, const char* msg) {
    std::fprintf(f, "[%s] [%s] %s\n", timestamp, name, msg);
}

static void logCallback(ChiakiLogLevel level, const char* msg, void* user) {
    const auto* name = static_cast<const char*>(user);
    if (!name || !msg) return;

    // These are ten-second aggregate records produced by the patched Chiaki
    // receive/video paths. Route them through the asynchronous sparse writer
    // even when APP_DIAG=0; never perform SD I/O on the Takion thread.
    if (std::strncmp(msg, "LUNARNX-PSRX ", 13) == 0 ||
        std::strncmp(msg, "LUNARNX-PSVIDEO ", 16) == 0) {
        lunar::dropDiagnosticLog("ps-transport", "%s", msg);
        return;
    }

    // Debug/verbose and per-packet diagnostics can starve the Switch UDP
    // receive loop. Drop them before touching the SD card. Keep this callback
    // deliberately stateless: it runs inside Chiaki's PSN and UDP threads.
    if (level == CHIAKI_LOG_DEBUG || level == CHIAKI_LOG_VERBOSE) return;
    const NoisyLogKind noisy = noisyLogKind(msg);
    if (noisy == NoisyLogKind::SendBufferOverflow) {
        const uint64_t count =
            g_send_buffer_overflow_count.fetch_add(1, std::memory_order_relaxed) + 1;
        // Preserve logarithmic evidence without opening the SD log on every
        // Chiaki network-thread overflow. 1..4 and powers of two produce only
        // 12 async records for the 1037-event hardware trace.
        if (count <= 4 || isPowerOfTwo(count)) {
            lunar::dropDiagnosticLog(
                "chiaki-flow", "%s count=%llu", msg,
                static_cast<unsigned long long>(count));
        }
        return;
    }
    if (noisy != NoisyLogKind::None) return;

#if LUNARNX_DIAGNOSTIC_LOG
    ensureDiagnosticLogDirectory();
    std::lock_guard<std::mutex> file_lock(diagnosticLogMutex());
    FILE* f = std::fopen(get_diagnostic_log_path(), "a");
    if (!f) return;
    char timestamp[32];
    diagnosticTimestamp(timestamp, sizeof(timestamp));
    writeLogLine(f, timestamp, name, msg);
    std::fclose(f);
#else
    (void)level;
#endif
}

ChiakiLog makeChiakiDiagnosticLog(const char* name) {
    ChiakiLog log{};
#if LUNARNX_DIAGNOSTIC_LOG || LUNARNX_DROP_DIAGNOSTIC_LOG
    log.level_mask = CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR;
    log.cb = logCallback;
    // Callers use process-lifetime string literals, so no callback context
    // allocation or cross-thread cleanup is required.
    log.user = const_cast<char*>(name);
#else
    (void)name;
#endif
    return log;
}

} // namespace lunar::ps

#endif
