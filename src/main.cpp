#ifdef __SWITCH__
#include <switch.h>
#include <borealis.hpp>
#include <cstdlib>
#include "diagnostics.h"
#include "app/runtime_context.h"
#include "ui/platform_activity.h"
#include "ui/i18n.h"
#include "ui/ui_style.h"
#if LUNARNX_PS_MOCK_AUTORUN
#include "tools/ps_mock_lifecycle_activity.h"
#endif

namespace {

s32 selectAllowedSwitchCore(u64 affinityMask, int ordinal) {
    s32 lastAllowedCore = -1;

    for (s32 core = 0; core < 4; core++) {
        if ((affinityMask & (1ULL << core)) == 0) {
            continue;
        }

        lastAllowedCore = core;
        if (ordinal == 0) {
            return core;
        }

        ordinal--;
    }

    return lastAllowedCore;
}

void preferSwitchCore(int ordinal) {
    s32 preferredCore = -1;
    u64 affinityMask = 0;

    if (R_FAILED(svcGetThreadCoreMask(&preferredCore, &affinityMask, CUR_THREAD_HANDLE))) {
        return;
    }

    s32 targetCore = selectAllowedSwitchCore(affinityMask, ordinal);
    if (targetCore >= 0 && targetCore != preferredCore) {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, targetCore, static_cast<u32>(affinityMask));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 0) lunar::app::setRunningNroPath(argv[0]);
    lunar::diagnosticLog("main", "begin argc=%d version=%s", argc, LUNARNX_VERSION);

    // Keep the Switch startup path aligned with Moonlight-Switch.
    appletInitializeGamePlayRecording();
    appletSetWirelessPriorityMode(AppletWirelessPriorityMode_OptimizedForWlan);
    preferSwitchCore(0);
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);
    lunar::diagnosticLog("main", "switch thread/app settings done");

    brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);

    lunar::ui::configureAppLocale();
    lunar::diagnosticLog("main", "borealis init begin");
    if (!brls::Application::init()) {
        lunar::diagnosticLog("main", "borealis init failed");
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }
    lunar::diagnosticLog("main", "borealis init done");
    lunar::ui::installLunarTheme();
    lunar::diagnosticLog("main", "createWindow begin");
    brls::Application::createWindow("LunarNX");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(false);
    brls::Application::setFPSStatus(false);
    lunar::diagnosticLog("main", "createWindow done");

    lunar::diagnosticLog("main", "push root activity begin");
#if LUNARNX_PS_MOCK_AUTORUN
    brls::Application::pushActivity(new lunar::tools::PsMockLifecycleActivity());
#else
    brls::Application::pushActivity(new lunar::ui::PlatformActivity());
#endif
    lunar::diagnosticLog("main", "push root activity done");

    lunar::diagnosticLog("main", "mainLoop begin");
    while (brls::Application::mainLoop()) {}
    lunar::diagnosticLog("main", "mainLoop end");

    return EXIT_SUCCESS;
}
#else
#include <cstdio>
int main(int argc, char* argv[]) {
    printf("[LunarNX] Desktop test mode\n");
    printf("  Run: ./build/pc/LunarNX_Test <command>\n");
    return 0;
}
#endif
