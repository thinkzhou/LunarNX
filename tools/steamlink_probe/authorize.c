#include "ihslib/client.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IHS_Client *active_client;
static const char *pin;
static int authorization_started;

static void on_discovered(IHS_Client *client, const IHS_HostInfo *host, void *context) {
    (void) context;
    if (authorization_started) return;
    authorization_started = 1;
    printf("discovered host=%s address=%u.%u.%u.%u:%u games_running=%s\n",
           host->hostname,
           host->address.ip.v4.data[0], host->address.ip.v4.data[1],
           host->address.ip.v4.data[2], host->address.ip.v4.data[3],
           host->address.port, host->gamesRunning ? "true" : "false");
    fflush(stdout);
    if (!IHS_ClientAuthorizationRequest(client, host, pin)) {
        fprintf(stderr, "authorization request could not be started\n");
        IHS_ClientStop(client);
    }
}

static void on_authorization_progress(IHS_Client *client, const IHS_HostInfo *host, void *context) {
    (void) client;
    (void) host;
    (void) context;
    printf("authorization in progress\n");
    fflush(stdout);
}

static void on_authorization_success(IHS_Client *client, const IHS_HostInfo *host,
                                     uint64_t steam_id, void *context) {
    (void) client;
    (void) host;
    (void) context;
    printf("authorization success steam_id=%llu\n", (unsigned long long) steam_id);
    fflush(stdout);
    IHS_ClientStop(active_client);
}

static void on_authorization_failed(IHS_Client *client, const IHS_HostInfo *host,
                                    IHS_AuthorizationResult result, void *context) {
    (void) client;
    (void) host;
    (void) context;
    printf("authorization failed result=%d\n", result);
    fflush(stdout);
    IHS_ClientStop(active_client);
}

static void on_signal(int signal_number) {
    (void) signal_number;
    if (active_client != NULL) IHS_ClientStop(active_client);
}

int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] == '\0') {
        fprintf(stderr, "usage: %s PAIRING_CODE\n", argv[0]);
        return 2;
    }
    pin = argv[1];
    signal(SIGINT, on_signal);

    static const uint8_t secret_key[32] = {
        0x4c, 0x75, 0x6e, 0x61, 0x72, 0x4e, 0x58, 0x2d,
        0x53, 0x74, 0x65, 0x61, 0x6d, 0x4c, 0x69, 0x6e,
        0x6b, 0x2d, 0x50, 0x72, 0x6f, 0x62, 0x65, 0x2d,
        0x32, 0x30, 0x32, 0x36, 0x2d, 0x30, 0x39, 0x30,
    };
    const IHS_ClientConfig config = {
        .deviceId = 0x4c4e58535445414dULL,
        .secretKey = secret_key,
        .deviceName = "LunarNX Steam Probe",
    };
    IHS_Init();
    active_client = IHS_ClientCreate(&config);
    if (active_client == NULL) {
        fprintf(stderr, "could not create ihslib client\n");
        IHS_Quit();
        return 1;
    }
    const IHS_ClientDiscoveryCallbacks discovery = {.discovered = on_discovered};
    const IHS_ClientAuthorizationCallbacks authorization = {
        .progress = on_authorization_progress,
        .success = on_authorization_success,
        .failed = on_authorization_failed,
    };
    IHS_ClientSetDiscoveryCallbacks(active_client, &discovery, NULL);
    IHS_ClientSetAuthorizationCallbacks(active_client, &authorization, NULL);
    printf("searching for Steam hosts; PIN=%s\n", pin);
    fflush(stdout);
    if (!IHS_ClientStartDiscovery(active_client, 1000)) {
        fprintf(stderr, "could not start discovery\n");
        IHS_ClientStop(active_client);
    }
    IHS_ClientThreadedJoin(active_client);
    IHS_ClientDestroy(active_client);
    active_client = NULL;
    IHS_Quit();
    return authorization_started ? 0 : 1;
}
