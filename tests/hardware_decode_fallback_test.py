#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    media_header = (ROOT / "src/stream/media_pipeline.h").read_text()
    decoder = (ROOT / "src/stream/video_decoder.cpp").read_text()
    pipeline = (ROOT / "src/stream/media_pipeline.cpp").read_text()

    require("usesHardwareDecode(VideoBackend backend)" in media_header,
            "Video backend helpers should identify both NVDEC modes")
    require("usesZeroCopyRender(VideoBackend backend)" in media_header,
            "Video backend helpers should identify the deko3d mode")
    require("ctx->extra_hw_frames = 16" in decoder,
            "NVDEC should keep a larger hardware frame pool")
    require("av_hwframe_transfer_data(sw_frame, frame, 0)" in decoder,
            "NVDEC frames should be transferred to CPU NV12 for the safe display path")
    require("hardware avcodec_send_packet ret=%d" in decoder,
            "Hardware decode should submit complete RTP access units directly")
    require("av_new_packet(pkt, static_cast<int>(len))" in decoder,
            "Hardware decode packets should be FFmpeg-owned and input-padded")
    require("auto receive_available = [&]()" in decoder,
            "Hardware decode should drain queued NVDEC frames after each send")
    require("if (ret == AVERROR(EAGAIN))" in decoder,
            "Hardware decode should retry only after explicit FFmpeg back-pressure")
    require("ret == AVERROR(EAGAIN) || ret == AVERROR_UNKNOWN" not in decoder,
            "NVDEC status errors must not retry partially consumed H.264 packets")
    require("NVDEC status error; packet not retried" in decoder,
            "NVDEC status errors should leave focused diagnostics")
    require("hardware avcodec_send_packet retry ret=%d" in decoder,
            "Hardware decode retry path should be logged")
    require("NVDEC transfer frame" in decoder,
            "NVDEC transfer path should leave focused diagnostics")
    require("usesHardwareDecode(video_backend_)" in decoder,
            "Both hardware modes should share NVDEC initialization and packet submission")
    require("video_backend_ == VideoBackend::HardwareCopyOut" in decoder,
            "Only hardware copy-out mode should transfer NVDEC frames to CPU")
    require("callback_frame->format == AV_PIX_FMT_NVTEGRA" in decoder,
            "Hardware frame detection should remain explicit")
    require("usesZeroCopyRender(options.video_backend)" in pipeline,
            "Only full hardware mode should select the zero-copy renderer")
    require("? VideoBackend::HardwareZeroCopy" in pipeline and
            ": VideoBackend::Software" in pipeline,
            "CPU frame modes should select the software renderer")
    require("video renderer backend=%s decode_backend=%s" in pipeline,
            "Renderer/decode backend split should be logged")

    print("Hardware decode fallback tests passed")


if __name__ == "__main__":
    main()
