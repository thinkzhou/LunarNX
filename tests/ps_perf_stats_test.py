#!/usr/bin/env python3
from pathlib import Path


def main():
    bridge = Path("src/ps/ps_media_bridge.cpp").read_text()
    pipeline = Path("src/stream/media_pipeline.cpp").read_text()
    overlay = Path("src/ui/stream_overlay.cpp").read_text()
    detail = Path("src/ui/perf_overlay.cpp").read_text()
    assert "recordIncomingVideoSample" in bridge
    assert "recordIncomingAudioPacket" in bridge
    assert "recordVideoPacket(bytes, pts_ns)" in pipeline
    assert "recordPackets(1, frames_lost)" in pipeline
    assert "packets_lost" in overlay and "Chiaki delivers complete samples" in overlay
    assert "interval_bitrate_mbps" in overlay
    assert "bitrate_samples_" in overlay
    assert "bitrate_mbps_ = ps_bitrate" not in overlay
    assert "packets_lost" in detail
    print("PS perf stats test passed")


if __name__ == "__main__":
    main()
