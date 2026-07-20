/*
    Copyright 2019 natinusala
    Copyright 2019 WerWolv
    Copyright 2019 p-sam

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

    LunarNX keeps this copy in project-owned source so startup diagnostics are
    tracked with the app instead of hidden in vendored code.
*/

#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <switch.h>
#include <unistd.h>

static int nxlink_sock = -1;
static bool socket_initialized = false;
static bool romfs_initialized = false;
static bool hidsys_initialized = false;
static bool inss_initialized = false;
static bool pl_initialized = false;
static bool setsys_initialized = false;
static bool set_initialized = false;
static bool psm_initialized = false;
static bool nifm_initialized = false;
static bool lbl_initialized = false;

static void switchEnsureLogDirectory(void)
{
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/LunarNX", 0777);
}

static void switchEarlyLog(const char* format, ...)
{
    switchEnsureLogDirectory();

    FILE* log = fopen("sdmc:/switch/LunarNX/lunarnx.log", "a");
    if (!log)
        return;

    fprintf(log, "[userAppInit] ");

    va_list args;
    va_start(args, format);
    vfprintf(log, format, args);
    va_end(args);

    fprintf(log, "\n");
    fclose(log);
}

static bool logInitResult(const char* name, Result rc)
{
    switchEarlyLog("%s rc=0x%08x", name, rc);
    return R_SUCCEEDED(rc);
}

void userAppInit()
{
    printf("userAppInit\n");
    switchEarlyLog("userAppInit begin");
    appletLockExit();
    switchEarlyLog("appletLockExit done");

    SocketInitConfig cfg = *(socketGetDefaultInitConfig());
    AppletType at        = appletGetAppletType();
    switchEarlyLog("applet type=%d", at);
    if (at == AppletType_Application || at == AppletType_SystemApplication)
    {
        cfg.num_bsd_sessions = 12;
        cfg.sb_efficiency    = 8;
        socket_initialized = logInitResult("socketInitialize application", socketInitialize(&cfg));
    }
    else
    {
        cfg.num_bsd_sessions = 2;
        cfg.sb_efficiency    = 1;
        socket_initialized = logInitResult("socketInitialize applet", socketInitialize(&cfg));
    }

#ifdef DEBUG
    nxlink_sock = nxlinkStdio();
    switchEarlyLog("nxlinkStdio result=%d", nxlink_sock);
#endif

    romfs_initialized = logInitResult("romfsInit", romfsInit());
    hidsys_initialized = logInitResult("hidsysInitialize", hidsysInitialize());
    inss_initialized = logInitResult("inssInitialize", inssInitialize());
    pl_initialized = logInitResult("plInitialize", plInitialize(PlServiceType_User));
    setsys_initialized = logInitResult("setsysInitialize", setsysInitialize());
    set_initialized = logInitResult("setInitialize", setInitialize());
    psm_initialized = logInitResult("psmInitialize", psmInitialize());
    nifm_initialized = logInitResult("nifmInitialize", nifmInitialize(NifmServiceType_User));
    lbl_initialized = logInitResult("lblInitialize", lblInitialize());
    switchEarlyLog("userAppInit done");
}

void userAppExit()
{
    printf("userAppExit\n");
    switchEarlyLog("userAppExit begin");

    if (lbl_initialized)
        lblExit();
    if (nifm_initialized)
        nifmExit();
    if (psm_initialized)
        psmExit();
    if (set_initialized)
        setExit();
    if (setsys_initialized)
        setsysExit();
    if (pl_initialized)
        plExit();
    if (inss_initialized)
        inssExit();
    if (hidsys_initialized)
        hidsysExit();
    if (romfs_initialized)
        romfsExit();

    if (nxlink_sock != -1)
        close(nxlink_sock);

    if (socket_initialized)
        socketExit();

    appletUnlockExit();
    switchEarlyLog("userAppExit done");
}
