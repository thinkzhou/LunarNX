#include "stream/perf_stats.h"

#include <chrono>
#include <cstdint>
#include <cstdio>

using Clock = std::chrono::steady_clock;

__attribute__((noinline))
void recordHotPathSamples(lunar::stream::PerfStats& perf, uint64_t sample) {
    perf.recordVideoAccessUnit(1400, sample, sample % 9000, false);
    perf.recordRenderQueueWait(sample % 17000);
    perf.recordPresentCall(sample % 3000, (sample & 1u) != 0);
    perf.recordInputSample(8000 + sample % 300, sample % 200);
    perf.recordInputSnapshotAge(sample % 8000);
}

int main() {
    constexpr uint64_t kIterations = 1'000'000;
    lunar::stream::PerfStats perf;
    perf.reset();
    const auto started = Clock::now();
    for (uint64_t i = 0; i < kIterations; ++i) {
        recordHotPathSamples(perf, i);
    }
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - started).count();
    const auto window = perf.takeLatencyWindow();
    std::printf("latency_diag=%d iterations=%llu ns_per_sample=%.2f samples=%u\n",
                LUNARNX_LATENCY_DIAGNOSTIC_LOG,
                static_cast<unsigned long long>(kIterations),
                static_cast<double>(elapsed_ns) /
                    static_cast<double>(kIterations),
                window.access_unit_queue_samples);
    return 0;
}
