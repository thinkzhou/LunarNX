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
#include <time.h>
#include <switch.h>
#include <unistd.h>

static int nxlink_sock = -1;
#include <fcntl.h>
#include <string.h>

#define SWITCH_RUNNING_MARKER "sdmc:/switch/LunarNX/.running"
#define SWITCH_CRASH_LOG "sdmc:/switch/LunarNX/crash.log"

extern char __start__;

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

    struct timespec now = {0};
    struct tm utc_time = {0};
    if (clock_gettime(CLOCK_REALTIME, &now) == 0 &&
        gmtime_r(&now.tv_sec, &utc_time) != NULL)
    {
        fprintf(log, "[%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ] ",
                utc_time.tm_year + 1900, utc_time.tm_mon + 1,
                utc_time.tm_mday, utc_time.tm_hour, utc_time.tm_min,
                utc_time.tm_sec, now.tv_nsec / 1000000L);
    }
    else
    {
        fprintf(log, "[0000-00-00T00:00:00.000Z] ");
    }
    fprintf(log, "[userAppInit] ");

    va_list args;
    va_start(args, format);
    vfprintf(log, format, args);
    va_end(args);

    fprintf(log, "\n");
    fclose(log);
}

static size_t crashAppendStr(char* buf, size_t pos, size_t cap, const char* s)
{
    while (*s && pos + 1 < cap)
        buf[pos++] = *s++;
    return pos;
}

static size_t crashAppendHex(char* buf, size_t pos, size_t cap, u64 value)
{
    static const char digits[] = "0123456789abcdef";
    pos = crashAppendStr(buf, pos, cap, "0x");
    for (int shift = 60; shift >= 0; shift -= 4)
    {
        if (pos + 1 >= cap)
            break;
        buf[pos++] = digits[(value >> shift) & 0xf];
    }
    return pos;
}

// Runs on libnx's exception stack after a fatal CPU fault. It avoids stdio and
// heap allocation because the fault may have happened while holding the malloc
// lock. Subtract code_base from pc/lr and feed the result to addr2line against
// the matching LunarNX.elf to find the faulting function.
void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
    char line[512];
    size_t pos = 0;
    const size_t cap = sizeof(line);

    pos = crashAppendStr(line, pos, cap, "[exception] desc=");
    pos = crashAppendHex(line, pos, cap, ctx->error_desc);
    pos = crashAppendStr(line, pos, cap, " code_base=");
    pos = crashAppendHex(line, pos, cap, (u64)(uintptr_t)&__start__);
    pos = crashAppendStr(line, pos, cap, " pc=");
    pos = crashAppendHex(line, pos, cap, ctx->pc.x);
    pos = crashAppendStr(line, pos, cap, " lr=");
    pos = crashAppendHex(line, pos, cap, ctx->lr.x);
    pos = crashAppendStr(line, pos, cap, " sp=");
    pos = crashAppendHex(line, pos, cap, ctx->sp.x);
    pos = crashAppendStr(line, pos, cap, " fp=");
    pos = crashAppendHex(line, pos, cap, ctx->fp.x);
    pos = crashAppendStr(line, pos, cap, " far=");
    pos = crashAppendHex(line, pos, cap, ctx->far.x);
    pos = crashAppendStr(line, pos, cap, " esr=");
    pos = crashAppendHex(line, pos, cap, ctx->esr);
    pos = crashAppendStr(line, pos, cap, "\n");

    int fd = open(SWITCH_CRASH_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0)
    {
        write(fd, line, pos);
        close(fd);
    }
}

static void switchMarkRunStart(void)
{
    switchEnsureLogDirectory();

    struct stat st;
    if (stat(SWITCH_RUNNING_MARKER, &st) == 0)
        switchEarlyLog("previous run ended abnormally (no clean exit marker); see crash.log");

    int fd = open(SWITCH_RUNNING_MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
        close(fd);
}

static void switchMarkCleanExit(void)
{
    remove(SWITCH_RUNNING_MARKER);
}

static bool logInitResult(const char* name, Result rc)
{
    switchEarlyLog("%s rc=0x%08x", name, rc);
    return R_SUCCEEDED(rc);
}

void userAppInit()
{
    printf("userAppInit\n");
    switchMarkRunStart();
    switchEarlyLog("userAppInit begin");
    appletLockExit();
    switchEarlyLog("appletLockExit done");

    const SocketInitConfig default_cfg = *(socketGetDefaultInitConfig());
    SocketInitConfig cfg = default_cfg;
    AppletType at        = appletGetAppletType();
    // 1080p/HQ streams can burst several large RTP packets per frame.  The
    // libnx default UDP receive buffer is only about 42 KiB, which lets the
    // kernel drop packets while the Switch is busy presenting a frame.
    // Keep the larger buffer in the dynamic socket heap; if a platform build
    // cannot reserve it, retry with libnx's original configuration. Keep
    // the known-good 512 KiB allocation so ICE/socket setup does not depend
    // on a large upfront BSD buffer reservation.
    cfg.udp_rx_buf_size = 512 * 1024;
    switchEarlyLog("applet type=%d", at);
    if (at == AppletType_Application || at == AppletType_SystemApplication)
    {
        cfg.num_bsd_sessions = 12;
        cfg.sb_efficiency    = 8;
        Result rc = socketInitialize(&cfg);
        switchEarlyLog(
            "socketInitialize application enhanced rc=0x%08x udp_rx=%u sessions=%u efficiency=%u",
            rc, cfg.udp_rx_buf_size, cfg.num_bsd_sessions, cfg.sb_efficiency);
        if (R_FAILED(rc))
        {
            rc = socketInitialize(&default_cfg);
            switchEarlyLog("socketInitialize application fallback rc=0x%08x", rc);
        }
        socket_initialized = R_SUCCEEDED(rc);
    }
    else
    {
        // Chiaki creates sockets from nested discovery/registration workers:
        // one UDP socket backs each stop pipe and another talks to the console.
        // Two BSD IPC sessions make the second worker fail immediately in
        // LibraryApplet mode even though socketInitialize itself succeeds.
        // Keep the low-efficiency buffer budget for Applet Mode, but provide
        // the same request concurrency as the known-good Application path.
        cfg.num_bsd_sessions = 12;
        cfg.sb_efficiency    = 1;
        Result rc = socketInitialize(&cfg);
        switchEarlyLog(
            "socketInitialize applet enhanced rc=0x%08x udp_rx=%u sessions=%u efficiency=%u",
            rc, cfg.udp_rx_buf_size, cfg.num_bsd_sessions, cfg.sb_efficiency);
        if (R_FAILED(rc))
        {
            rc = socketInitialize(&default_cfg);
            switchEarlyLog("socketInitialize applet fallback rc=0x%08x", rc);
        }
        socket_initialized = R_SUCCEEDED(rc);
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
    switchMarkCleanExit();
    switchEarlyLog("userAppExit done");
}
