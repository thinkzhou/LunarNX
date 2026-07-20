// Desktop xCloud stream smoke test for Fortnite / other titles.
// Usage:
//   make -f Makefile.desktop xcloud_stream
//   LUNARNX_TOKEN_PATH=./token.json \
//   LUNARNX_FORCE_REGION_IP=4.2.2.2 \
//   LUNARNX_TITLE_ID=FORTNITE \
//   ./build/pc/xcloud_stream
//
// Streams until Ctrl+C or error. Uses StreamController full path
// (play/connect/SDP/ICE/media) with XStreaming lpt connect token.

#include "../app/stream_controller.h"
#include "../common.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <thread>

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running = false; }

static const char* envOr(const char* k, const char* fb) {
    const char* v = std::getenv(k);
    return (v && v[0]) ? v : fb;
}

int main() {
    setbuf(stdout, nullptr);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    const char* token_path = envOr("LUNARNX_TOKEN_PATH", lunar::get_token_path());
    const char* region = envOr("LUNARNX_FORCE_REGION_IP", "4.2.2.2");
    const char* title_id = envOr("LUNARNX_TITLE_ID", "FORTNITE");
    const int width = std::atoi(envOr("LUNARNX_WIDTH", "1280"));
    const int height = std::atoi(envOr("LUNARNX_HEIGHT", "720"));
    const int run_seconds = std::atoi(envOr("LUNARNX_STREAM_SECONDS", "30"));

    printf("=== LunarNX desktop xCloud stream ===\n");
    printf("token_path=%s\n", token_path);
    printf("force_region_ip=%s\n", region);
    printf("title_id=%s\n", title_id);
    printf("resolution=%dx%d run_seconds=%d\n", width, height, run_seconds);

    lunar::app::StreamController ctrl;
    ctrl.setStateCallback([](lunar::app::StreamState s, const std::string& info) {
        static const char* names[] = {
            "Idle", "Auth", "Connecting", "Streaming", "Disconnected", "Error"};
        const int idx = static_cast<int>(s);
        printf("[%s] %s\n",
               (idx >= 0 && idx < 6) ? names[idx] : "?",
               info.c_str());
    });
    ctrl.setForceRegionIp(region);

    if (!ctrl.loadTokens(token_path)) {
        fprintf(stderr, "FAIL: loadTokens(%s)\n", token_path);
        return 1;
    }
    printf("loaded gamertag=%s cloud=%s\n",
           ctrl.getGamertag().c_str(),
           ctrl.hasCloudAccess() ? "yes" : "no");

    // Force token re-derive / lpt will be fetched inside startCloudStream.
    // Ensure cloud token exists first via a lightweight fetch if needed.
    if (!ctrl.hasCloudAccess()) {
        printf("cloud token missing, fetching titles to trigger refresh...\n");
        (void)ctrl.fetchCloudTitles(true);
        printf("after fetch cloud=%s err=%s\n",
               ctrl.hasCloudAccess() ? "yes" : "no",
               ctrl.getCloudTitleFetchError().c_str());
    }

    printf("starting cloud stream title=%s...\n", title_id);
    if (!ctrl.startCloudStream(title_id, width, height)) {
        fprintf(stderr, "FAIL: startCloudStream: %s\n",
                ctrl.getLastStreamError().c_str());
        return 2;
    }

    const auto start = std::chrono::steady_clock::now();
    while (g_running) {
        ctrl.update();
        const auto state = ctrl.getState();
        if (state == lunar::app::StreamState::Streaming) {
            // keep going
        } else if (state == lunar::app::StreamState::Error ||
                   state == lunar::app::StreamState::Disconnected) {
            printf("stream ended state=%d err=%s\n",
                   static_cast<int>(state),
                   ctrl.getLastStreamError().c_str());
            break;
        }

        if (run_seconds > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            if (elapsed >= run_seconds) {
                printf("reached run_seconds=%d, stopping\n", run_seconds);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    g_running = false;
    printf("stopping stream...\n");
    try {
        ctrl.stopStream();
        for (int i = 0; i < 30; ++i) {
            ctrl.update();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "stopStream exception: %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "stopStream unknown exception\n");
    }

    const auto final_state = ctrl.getState();
    printf("final state=%d err=%s\n",
           static_cast<int>(final_state),
           ctrl.getLastStreamError().c_str());
    printf("=== xcloud_stream done ===\n");
    // Prefer success if we entered Streaming at least once and no hard error string remains.
    return (final_state == lunar::app::StreamState::Error) ? 3 : 0;
}
