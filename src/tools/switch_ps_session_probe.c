#include <switch.h>
#include <chiaki/random.h>
#include <chiaki/session.h>
#include <chiaki/log.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* log_file = NULL;

static void log_line(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    if (log_file)
        vfprintf(log_file, fmt, args);
    fprintf(stderr, fmt, args);
    va_end(args);
    if (log_file)
        fflush(log_file);
}

static void probe_log_cb(ChiakiLogLevel level, const char* msg, void* user)
{
    (void)user;
    log_line("[chiaki:%c] %s\n", chiaki_log_level_char(level), msg);
}

static void run_tests(void)
{
    log_line("[probe] run_tests begin\n");

    /* 1. randomGet (libnx CSPRNG) */
    {
        uint8_t buf[16];
        randomGet(buf, sizeof(buf));
        log_line("[probe] randomGet OK: %02x%02x%02x%02x...\n",
                 buf[0], buf[1], buf[2], buf[3]);
    }

    /* 2. chiaki_random_bytes_crypt (mbedTLS CTR_DRBG + hardware entropy) */
    {
        uint8_t buf[32];
        ChiakiErrorCode err = chiaki_random_bytes_crypt(buf, sizeof(buf));
        log_line("[probe] chiaki_random_bytes_crypt rc=%d data=%02x%02x%02x%02x...\n",
                 err, buf[0], buf[1], buf[2], buf[3]);
    }

    /* 3. chiaki_session_init with a minimal LAN connect_info */
    {
        log_line("[probe] step3 begin\n");
        ChiakiLog log;
        chiaki_log_init(&log, CHIAKI_LOG_ALL, probe_log_cb, NULL);
        log_line("[probe] step3.1 log init done\n");

        ChiakiConnectInfo connect_info = {0};
        connect_info.ps5 = true;
        connect_info.host = "192.168.31.127";
        log_line("[probe] step3.2 host set\n");
        connect_info.video_profile.width = 1280;
        connect_info.video_profile.height = 720;
        connect_info.video_profile.max_fps = 60;
        connect_info.video_profile.bitrate = 10000;
        connect_info.video_profile.codec = CHIAKI_CODEC_H264;
        connect_info.video_profile_auto_downgrade = true;
        connect_info.packet_loss_max = 0.02;
        connect_info.auto_regist = false;
        log_line("[probe] step3.3 profile set\n");

        log_line("[probe] calling chiaki_session_init (LAN)\n");
        ChiakiSession session;
        ChiakiErrorCode err = chiaki_session_init(&session, &connect_info, &log);
        log_line("[probe] chiaki_session_init rc=%d\n", err);
        if (err == CHIAKI_ERR_SUCCESS)
        {
            chiaki_session_fini(&session);
            log_line("[probe] session init + fini OK\n");
        }
    }

    log_line("[probe] done\n");
}

static void worker_thread(void* arg)
{
    (void)arg;
    run_tests();
    svcSleepThread(100000000LL);
    exit(0);
}

int main(void)
{
    consoleInit(nullptr);

    log_file = fopen("sdmc:/switch/ps_session_probe.log", "w");
    if (!log_file)
        log_file = stderr;

    log_line("[probe] PS session probe start\n");

    Thread thread;
    const size_t stack_size = 8 * 1024 * 1024;
    void* stack_mem = aligned_alloc(0x1000, stack_size);
    if (!stack_mem)
    {
        log_line("[probe] stack alloc failed\n");
        return 1;
    }
    threadCreate(&thread, worker_thread, NULL, stack_mem, stack_size, 0x30, -2);
    threadStart(&thread);

    while (appletMainLoop())
    {
        consoleUpdate(nullptr);
        svcSleepThread(50 * 1000 * 1000LL);
    }

    threadWaitForExit(&thread);
    threadClose(&thread);
    free(stack_mem);

    if (log_file != stderr)
        fclose(log_file);

    consoleExit(nullptr);
    return 0;
}
