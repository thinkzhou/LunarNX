/**
 * LunarNX Desktop Test Runner
 *
 * Full-stack Xbox streaming test on macOS/Linux using SDL2 for video/audio/keyboard.
 * Validates the entire pipeline (auth → API → WebRTC → decode → render → input)
 * without needing Switch hardware.
 *
 * Build:
 *   mkdir -p build/pc && cd build/pc
 *   cmake ../.. -DPLATFORM_DESKTOP=ON -DCMAKE_BUILD_TYPE=Debug
 *   make LunarNX_Desktop -j$(sysctl -n hw.ncpu)
 *
 * Usage:
 *   ./LunarNX_Desktop
 *
 * Keyboard controls (during streaming):
 *   Enter=A  Space=B  X/Y=X/Y    Arrow keys=D-pad
 *   WASD=Left stick    IJKL=Right stick
 *   [=LB  ]=RB  Q=LT  E=RT
 *   Tab=View  Esc=Menu/Exit   Z/C=L3/R3
 */

#include "../app/stream_controller.h"
#include <cstdio>
#include <thread>
#include <signal.h>

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running = false; }

int main() {
    setbuf(stdout, NULL);  // unbuffered for test output
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    printf("=== LunarNX Desktop Test v0.1 ===\n\n");

    lunar::app::StreamController ctrl;

    ctrl.setStateCallback([](lunar::app::StreamState s, const std::string& info) {
        static const char* names[] = {"Idle","Auth","Connecting","Streaming","Disconnected","Error"};
        printf("[%s] %s\n", names[(int)s], info.c_str());
    });

    const char* mock_url = getenv("LUNARNX_MOCK_URL");

    if (mock_url && mock_url[0]) {
        // --- Mock mode: skip real auth, use local mock server ---
        printf("[mock] Bypassing auth, using mock Xbox at %s\n", mock_url);
        if (!ctrl.bypassAuthForMock(mock_url)) {
            fprintf(stderr, "ERROR: Failed to initialize mock mode.\n");
            return 1;
        }
    } else {
        // --- Normal mode: full Microsoft auth ---
        printf("Starting authentication...\n");
        if (!ctrl.startAuth()) {
            fprintf(stderr, "ERROR: Failed to start authentication.\n");
            return 1;
        }

        printf("\n  Visit: %s\n", ctrl.getAuthUrl().c_str());
        printf("  Code:  %s\n\n", ctrl.getAuthCode().c_str());
        printf("Press Enter AFTER completing the browser login...\n");
        getchar();

        printf("Polling for token...\n");
        if (!ctrl.pollAuth()) {
            fprintf(stderr, "ERROR: Authentication failed.\n");
            return 1;
        }
        printf("Logged in as: %s\n\n", ctrl.getGamertag().c_str());

        printf("Fetching console list...\n");
        if (!ctrl.fetchConsoles()) {
            fprintf(stderr, "ERROR: No consoles found. Is your Xbox online?\n");
            return 1;
        }
    }

    // Console list
    auto consoles = ctrl.getConsoles();
    for (size_t i = 0; i < consoles.size(); i++) {
        printf("  [%zu] %s  (%s)\n", i, consoles[i].name.c_str(), consoles[i].id.c_str());
    }

    size_t choice = 0;
    if (consoles.size() > 1) {
        printf("\nChoose console [0-%zu]: ", consoles.size() - 1);
        scanf("%zu", &choice);
        getchar();
    }

    // Connect & stream
    printf("\nConnecting to %s...\n", consoles[choice].name.c_str());
    if (!ctrl.startStream(consoles[choice].id, 1280, 720)) {
        fprintf(stderr, "ERROR: Failed to start stream.\n");
        return 1;
    }

    printf("\n=== STREAMING ===\n");
    printf("Keyboard: Enter=A Space=B X/Y=X/Y  Arrows=D-pad  WASD=LStick  IJKL=RStick\n");
    printf("          [=LB  ]=RB  Q=LT  E=RT  Tab=View  Esc=Stop  Z/C=L3/R3\n\n");

    // Step 4: Main loop (video/audio/input handled by stream_thread internally)
    while (g_running) {
        ctrl.update();

        auto state = ctrl.getState();
        if (state == lunar::app::StreamState::Disconnected ||
            state == lunar::app::StreamState::Error) {
            printf("Stream ended (state=%d)\n", (int)state);
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    printf("\nShutting down...\n");
    ctrl.stopStream();
    printf("Done.\n");
    return 0;
}
