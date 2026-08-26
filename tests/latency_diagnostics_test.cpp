#include "diagnostics.h"
#include "stream/perf_stats.h"

#include <cassert>
#include <chrono>
#include <thread>

int main() {
    lunar::stream::PerfStats perf;
    perf.reset();
    perf.recordVideoAccessUnit(1024, 1, 1200, false);
    perf.recordVideoAccessUnit(2048, 2, 3400, true);
    perf.recordDecodeLatency(2100);
    perf.recordDecodeLatency(4900);
    perf.recordRendererEnqueue(600);
    perf.recordRenderQueueWait(8000);
    perf.recordPresentCall(700, true);
    perf.recordInputSample(8100, 90);
    perf.recordInputSnapshotAge(1700);

    const auto first = perf.takeLatencyWindow();
    assert(first.access_unit_queue_total_us == 4600);
    assert(first.access_unit_queue_max_us == 3400);
    assert(first.access_unit_queue_samples == 2);
    assert(first.decode_total_us == 7000);
    assert(first.decode_max_us == 4900);
    assert(first.decode_samples == 2);
    assert(first.renderer_enqueue_total_us == 600);
    assert(first.render_queue_total_us == 8000);
    assert(first.presented_new_frames == 1);
    assert(first.input_sample_gap_max_us == 8100);
    assert(first.input_snapshot_age_total_us == 1700);

    const auto reset = perf.takeLatencyWindow();
    assert(reset.access_unit_queue_samples == 0);
    assert(reset.decode_samples == 0);
    assert(reset.render_queue_samples == 0);
    assert(reset.input_snapshot_age_samples == 0);

    lunar::startDropDiagnosticWriter();
    for (int index = 0; index < 40; ++index) {
        lunar::latencyDiagnosticLog("test", "window=%d value=%d", index,
                                    42 + index);
    }
    lunar::persistentEventLog("test", "persistent value=%d", 7);
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    const auto writer = lunar::asyncDiagnosticWriterStats();
    assert(writer.enqueued >= 41);
    assert(writer.dropped == 0);
    assert(writer.batches >= 1);
    assert(writer.bytes_written > 0);
    assert(writer.file_opens == 1);
    assert(writer.flushes >= 1);
    assert(writer.batches < writer.enqueued / 4);
    lunar::stopDropDiagnosticWriter();
    return 0;
}
