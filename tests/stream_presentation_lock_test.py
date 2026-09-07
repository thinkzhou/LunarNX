#!/usr/bin/env python3
"""Exercise production presentation lock acquisition against a GPU-owning UI.

Compile the PS presentation method and the media presentation prologue verbatim
with lightweight media stubs. The renderer is outside this test: the regression
occurs before it is reached, while shutdown waits for the UI's GPU mutex.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


ps = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()
media = (ROOT / "src/stream/media_pipeline.cpp").read_text()
view = (ROOT / "src/ui/stream_view.cpp").read_text()
hardware_view = method(view, "class HardwareVideoView") + ";"
ps_present = method(ps, "void PsStreamController::presentVideoFrame()")
media_present = method(media, "void MediaPipeline::presentVideoFrame()")
# Preserve the actual lock and any early-return checks. Replace only GPU work.
media_prologue = media_present.split("if (running_.load() && video_renderer_)", 1)[0]
source = r'''
#include <atomic>
#include <cassert>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
struct MediaPipeline {
    std::recursive_mutex lifecycle_mutex_;
    std::atomic<bool> running_{true};
    bool video_renderer_ = true;
    unsigned presented = 0;
    void presentVideoFrame();
};
struct PsStreamController {
    std::shared_mutex stream_operation_mutex_;
    std::shared_ptr<MediaPipeline> media_ = std::make_shared<MediaPipeline>();
    void presentVideoFrame();
};
''' + media_prologue + r'''
    if (running_.load() && video_renderer_) ++presented;
}
''' + ps_present + r'''
struct NVGcontext {};
namespace brls {
struct Style {};
struct FrameContext {};
struct View {
    virtual ~View() = default;
    virtual void draw(NVGcontext*, float, float, float, float,
                      Style, FrameContext*) = 0;
};
}
namespace app {
struct IStreamRuntime {
    unsigned presented = 0;
    void presentVideoFrame() { ++presented; }
};
}
''' + hardware_view + r'''
int main() {
    auto runtime = std::make_shared<app::IStreamRuntime>();
    auto terminal_stop = std::make_shared<std::atomic<bool>>(false);
    HardwareVideoView view(runtime, terminal_stop);
    view.draw(nullptr, 0, 0, 0, 0, {}, nullptr);
    assert(runtime->presented == 1);
    terminal_stop->store(true);
    view.draw(nullptr, 0, 0, 0, 0, {}, nullptr);
    assert(runtime->presented == 1);
    for (bool media_lock : {false, true}) {
        for (int round = 0; round < 25; ++round) {
            PsStreamController controller;
            std::recursive_mutex gpu;
            std::promise<void> stop_ready;
            auto ready = stop_ready.get_future();
            std::unique_lock<std::recursive_mutex> frame(gpu);
            std::thread stop([&] {
                std::unique_lock<std::shared_mutex> operation(
                    controller.stream_operation_mutex_, std::defer_lock);
                std::unique_lock<std::recursive_mutex> lifecycle(
                    controller.media_->lifecycle_mutex_, std::defer_lock);
                if (media_lock) lifecycle.lock(); else operation.lock();
                stop_ready.set_value();
                // Match shutdown: hold lifetime ownership, then acquire GPU.
                std::lock_guard<std::recursive_mutex> release_renderer(gpu);
            });
            ready.wait();
            // Match Borealis: hold GPU across the presentation callback.
            controller.presentVideoFrame();
            assert(controller.media_->presented == 0);
            frame.unlock();
            stop.join();
            // Contention must skip only this frame, not disable future video.
            controller.presentVideoFrame();
            assert(controller.media_->presented == 1);
            controller.media_->running_ = false;
            controller.presentVideoFrame();
            assert(controller.media_->presented == 1);
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix="lunarnx-presentation-lock-") as temp:
    cpp = Path(temp) / "test.cpp"
    binary = Path(temp) / "test"
    cpp.write_text(source)
    command = [os.environ.get("CXX", "c++"), "-std=c++17", "-pthread"]
    if sys.platform == "darwin":
        sdk = Path(subprocess.check_output(
            ["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip())
        command += ["-isysroot", str(sdk)]
        # Prefer the compiler's own libc++. Some Command Line Tools installs
        # omit its search path; only then use headers from the SAME SDK.
        probe = subprocess.run(
            command + ["-x", "c++", "-fsyntax-only", "-"],
            input="#include <mutex>\n", text=True, capture_output=True)
        libcxx = sdk / "usr/include/c++/v1"
        if probe.returncode != 0 and libcxx.exists():
            command += ["-isystem", str(libcxx)]
    subprocess.run(command + [str(cpp), "-o", str(binary)], check=True)
    try:
        subprocess.run([str(binary)], check=True, timeout=5)
    except subprocess.TimeoutExpired:
        raise SystemExit("FAIL: presentation deadlocked with GPU-waiting shutdown")
print("Presentation shutdown lock regression passed (50 forced interleavings)")
