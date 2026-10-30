// Real native full-pipeline smoke against tools/mock_xbox (not a unit mock).
#include "app/stream_controller.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    // The caller must supply a new empty generated directory. Never write the
    // developer's normal auth/config files when exercising backend switches.
    const auto directory = std::filesystem::absolute(argv[2]);
    if (std::filesystem::exists(directory)) return 2;
    std::filesystem::create_directories(directory);
    std::filesystem::current_path(directory);
    std::setbuf(stdout, nullptr);
    lunar::app::StreamController controller;
    if (!controller.bypassAuthForMock(argv[1])) return 1;
    const auto consoles = controller.getConsoles();
    if (consoles.empty()) return 1;
    struct Trial { const char* backend; bool cloud; };
    const Trial trials[] = {{"libdatachannel", false}, {"libpeer", false},
                            {"libdatachannel", true}, {"libdatachannel", false}};
    for (size_t cycle = 0; cycle < std::size(trials); ++cycle) {
        const auto trial = trials[cycle];
        {
            std::ofstream config("config.json");
            config << "{\"xbox_webrtc_backend\":\"" << trial.backend << "\"}";
        }
        std::printf("START cycle=%zu backend=%s cloud=%d\n", cycle, trial.backend, trial.cloud);
        const bool started = trial.cloud
            ? controller.startCloudStream("9NBLGGH537BL", 1280, 720)
            : controller.startStream(consoles.front().id, 1280, 720);
        if (!started) {
            std::fprintf(stderr, "start failed: %s\n", controller.getLastStreamError().c_str());
            controller.stopStream(); return 1;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        const auto minimum = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            controller.update();
            const auto& stats = controller.getPerfStats();
            if (std::chrono::steady_clock::now() >= minimum && stats.video_frames >= 60 &&
                stats.audio_frames >= 60 && stats.input_packets >= 60) break;
            if (controller.getState() == lunar::app::StreamState::Error) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        const auto& stats = controller.getPerfStats();
        const bool passed = stats.video_frames >= 60 && stats.audio_frames >= 60 && stats.input_packets >= 60;
        std::printf("RESULT cycle=%zu backend=%s cloud=%d video=%u audio=%u input=%u passed=%d\n",
            cycle, trial.backend, trial.cloud, stats.video_frames.load(),
            stats.audio_frames.load(), stats.input_packets.load(), passed);
        const auto stop_started = std::chrono::steady_clock::now();
        controller.stopStream();
        const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - stop_started).count();
        std::printf("STOP cycle=%zu milliseconds=%lld\n", cycle, static_cast<long long>(stop_ms));
        if (!passed || stop_ms > 10000) return 1;
    }
    return 0;
}
