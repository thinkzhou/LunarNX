#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    media_header = Path("src/stream/media_pipeline.h").read_text()
    decoder_header = Path("src/stream/video_decoder.h").read_text()
    decoder_impl = Path("src/stream/video_decoder.cpp").read_text()
    renderer_header = Path("src/stream/video_renderer.h").read_text()
    renderer_impl = Path("src/stream/video_renderer.cpp").read_text()
    stream_view = Path("src/ui/stream_view.cpp").read_text()
    main_header = Path("src/ui/main_activity.h").read_text()
    main_impl = Path("src/ui/main_activity.cpp").read_text()
    settings_header = Path("src/ui/stream_settings_activity.h").read_text()
    settings_impl = Path("src/ui/stream_settings_activity.cpp").read_text()
    loading_impl = Path("src/ui/stream_loading_activity.cpp").read_text()
    auth_impl = Path("src/ui/auth_activity.cpp").read_text()
    controller_header = Path("src/app/stream_controller.h").read_text()
    controller_impl = Path("src/app/stream_controller.cpp").read_text()
    switch_makefile = Path("Makefile.switch").read_text()
    cmake = Path("CMakeLists.txt").read_text()
    main_cpp = Path("src/main.cpp").read_text()
    config = Path("config/default_config.json").read_text()

    require("enum class VideoBackend" in media_header,
            "Media options should define a selectable video backend")
    require("HardwareZeroCopy" in media_header and
            "HardwareCopyOut" in media_header and
            "Software" in media_header,
            "Video backend should expose both hardware modes and software")
    require("VideoBackend video_backend = VideoBackend::HardwareZeroCopy" in media_header,
            "Default media options should select full hardware decode")

    require("setVideoBackend(VideoBackend backend)" in decoder_header,
            "VideoDecoder should accept the selected backend before initialize")
    require("usesHardwareDecode(video_backend_)" in decoder_impl,
            "Switch decoder should keep NVDEC behind both hardware backends")
    require("Software H.264 decoder initialized" in decoder_impl,
            "Switch software backend should reuse FFmpeg software H.264 decode")
    require("software alloc probe" in decoder_impl,
            "Switch software decoder should log allocation probes before FFmpeg init")
    require("softwareDecoderThreadCount()" in decoder_impl and
            "ctx->thread_count = softwareDecoderThreadCount();" in decoder_impl,
            "Switch software decoder should avoid FFmpeg frame threads in Ryujinx")
    require("avcodec_alloc_context3(codec)" in decoder_impl,
            "Software decoder should match rho's FFmpeg context allocation")
    require("AVCodecContext* ctx = avcodec_alloc_context3(nullptr)" not in decoder_impl,
            "Software decoder should not allocate a codec-less FFmpeg context")

    require("setVideoBackend(VideoBackend backend)" in renderer_header,
            "VideoRenderer should accept the selected backend before initialize")
    require("#include <libswscale/swscale.h>" in renderer_impl,
            "Switch software renderer should use swscale for YUV to RGBA")
    require("SoftwareVideoFrameSink::instance().publishRgba" in renderer_impl,
            "Switch software renderer should publish RGBA frames for Borealis")

    require("SoftwareVideoView" in stream_view,
            "StreamView should include a NanoVG view for software-rendered frames")
    require("SoftwareVideoFrameSink::instance().snapshot" in stream_view,
            "SoftwareVideoView should draw the latest published RGBA frame")

    require("StreamSettingsActivity" in settings_header and
            "new StreamSettingsActivity" in main_impl and
            "video_backend_btn_" not in main_header,
            "MainActivity should expose decoder selection through dedicated settings")
    require('"lunarnx/settings/decoder"' in settings_impl and
            '"lunarnx/settings/decoder_hardware"' in settings_impl and
            '"lunarnx/settings/decoder_copy"' in settings_impl and
            '"lunarnx/settings/decoder_software"' in settings_impl,
            "Stream settings should show all selected decoder backends")
    require("request.options.video_backend = video_backend_" in main_impl,
            "Selected decoder backend should be passed into startStream")
    require("kConnectStreamStackSize" in loading_impl and
            "8 * 1024 * 1024" in loading_impl,
            "Connect stream worker should have enough stack for FFmpeg software init")

    require("setDefaultVideoBackend" in controller_header and
            "getDefaultVideoBackend" in controller_header,
            "StreamController should store the configured default video backend")
    require("default_video_backend_" in controller_impl,
            "StreamController should persist the default video backend choice")
    require("video_backend" in auth_impl and "setDefaultVideoBackend" in auth_impl,
            "AuthActivity should parse video_backend from config.json")
    require('"video_backend": "hardware_zero_copy"' in config,
            "Default config should document the full hardware default")

    require("src/stream/software_video_frame.cpp" in switch_makefile,
            "Switch build should compile the software frame sink")
    require("src/platform/switch_heap.c" not in switch_makefile and
            "src/platform/switch_heap.c" not in cmake,
            "Switch builds should use libnx's dynamic default heap initializer")
    require("-lswscale" in switch_makefile,
            "Switch main target should link swscale for software video")
    require("lunarnx_switch_heap" not in main_cpp,
            "Switch startup should not depend on a custom heap initializer")

    print("Software video backend tests passed")


if __name__ == "__main__":
    main()
