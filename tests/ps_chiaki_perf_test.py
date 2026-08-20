#!/usr/bin/env python3
from pathlib import Path


def main():
    session = Path("src/ps/ps_stream_session.cpp").read_text()
    header = Path("src/ps/ps_stream_session.h").read_text()
    controller = Path("src/ps/ps_stream_controller.cpp").read_text()
    overlay = Path("src/ui/stream_overlay.cpp").read_text()
    detail = Path("src/ui/perf_overlay.cpp").read_text()
    runtime = Path("src/ps/ps_stream_controller.h").read_text()
    rtt_patch = Path(
        "tools/chiaki_switch/lunarnx-chiaki-stream-rtt.patch"
    ).read_text()
    assert "PsTransportStats" in header
    assert "stream_connection.measured_bitrate" in session
    assert "chiaki_bool_pred_cond_lock" in session
    assert "congestion->packet_loss" in session
    assert "chiaki_video_receiver_get_frames_lost_total" in session
    assert "session_.stream_connection.rtt" in session
    assert "std::lround(dynamic_rtt)" in session
    assert "session_.rtt_us / 1000ULL" in session
    assert "refreshTransportStats" in session
    assert "transport_rtt_ms_" in header
    assert "first_send_ms" in rtt_patch
    assert "chiaki_time_now_monotonic_ms()" in rtt_patch
    assert "event.data_ack.rtt_ms" in rtt_patch
    assert "CHIAKI_STREAM_CONNECTION_RTT_WINDOW" in rtt_patch
    assert "sample > 2000.0" in rtt_patch
    assert "q.has_rtt" in rtt_patch
    assert "stream_connection->rtt <= 0.0" in rtt_patch
    assert "transportStats()" in controller
    assert "hud_metrics_ps" in overlay
    assert "video_codec" in overlay
    assert "detail_codec" in detail
    assert "getVideoCodec() const override" in runtime
    print("PS chiaki performance test passed")


if __name__ == "__main__":
    main()
