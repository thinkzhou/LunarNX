#!/usr/bin/env python3
"""Source contracts for keeping the video backend in detailed diagnostics only."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    media = (ROOT / "src/stream/media_pipeline.h").read_text()
    overlay_header = (ROOT / "src/ui/stream_overlay.h").read_text()
    overlay = (ROOT / "src/ui/stream_overlay.cpp").read_text()
    perf_header = (ROOT / "src/ui/perf_overlay.h").read_text()
    perf_overlay = (ROOT / "src/ui/perf_overlay.cpp").read_text()
    view = (ROOT / "src/ui/stream_view.cpp").read_text()

    require("videoBackendOverlayName(VideoBackend backend)" in media,
            "Video backend should expose a compact overlay label")
    require('return "HW-ZC"' in media and
            'return "HW-Copy"' in media and
            'return "SW"' in media,
            "All three video modes should have compact overlay labels")
    require("video_backend" not in overlay_header and
            "video_backend" not in overlay,
            "Always-visible XStreaming-style HUD should not show the backend")
    require("const std::string& video_backend" in perf_header and
            '"lunarnx/perf/detail_backend"' in perf_overlay and
            'video_backend' in perf_overlay,
            "Detailed diagnostics should retain the active video backend")
    require("videoBackendOverlayName(" in view and
            "perf_overlay_->update(fps, res, video_backend)" in view,
            "Stream view should pass the selected mode only to detailed diagnostics")

    print("Video backend overlay tests passed")


if __name__ == "__main__":
    main()
