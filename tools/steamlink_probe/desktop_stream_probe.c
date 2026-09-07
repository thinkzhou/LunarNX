#include "ihslib.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const uint8_t secret_key[32] = {
    0x4c, 0x75, 0x6e, 0x61, 0x72, 0x4e, 0x58, 0x2d,
    0x53, 0x74, 0x65, 0x61, 0x6d, 0x4c, 0x69, 0x6e,
    0x6b, 0x2d, 0x50, 0x72, 0x6f, 0x62, 0x65, 0x2d,
    0x32, 0x30, 0x32, 0x36, 0x2d, 0x30, 0x39, 0x30,
};

static const IHS_ClientConfig client_config = {
    .deviceId = 0x4c4e58535445414dULL,
    .secretKey = secret_key,
    .deviceName = "LunarNX Steam Probe",
};

typedef struct ProbeContext {
    bool requested;
    bool authorization_requested;
    bool authorization_succeeded;
    bool succeeded;
    IHS_SocketAddress video_address;
    IHS_SessionInfo session_info;
    unsigned video_frames;
    unsigned audio_packets;
    size_t video_bytes;
    size_t audio_bytes;
    FILE *video_file;
    FILE *audio_file;
    int video_fifo;
    pid_t ffplay_pid;
    const char *video_path;
    const char *audio_path;
    const char *stream_pin;
    const char *pairing_pin;
    bool display;
} ProbeContext;

static IHS_Client *active_client;
static IHS_Session *active_session;

static void log_print(IHS_LogLevel level, const char *tag, const char *message) {
    fprintf(stderr, "[IHS.%s %s] %s\n", tag, IHS_LogLevelName(level), message);
}

static void stop_client(IHS_Client *client) {
    if (client != NULL) IHS_ClientStop(client);
}

static void request_stream(IHS_Client *client, const IHS_HostInfo *host, ProbeContext *probe) {
    IHS_StreamingRequest request = {
        .pin = "",
        .streamingEnable = {true, true, true},
        .maxResolution = {1280, 720},
        .audioChannelCount = 2,
        .streamingInterface = IHS_StreamInterfaceBigPicture,
    };
    if (probe->stream_pin != NULL) {
        snprintf(request.pin, sizeof(request.pin), "%s", probe->stream_pin);
    }
    if (!IHS_ClientStreamingRequest(client, host, &request)) {
        fprintf(stderr, "streaming request could not be started\n");
        stop_client(client);
    }
}

static void on_discovered(IHS_Client *client, const IHS_HostInfo *host, void *context) {
    ProbeContext *probe = context;
    if (probe->requested) return;
    probe->requested = true;
    printf("discovered host=%s address=%u.%u.%u.%u:%u games_running=%s\n",
           host->hostname,
           host->address.ip.v4.data[0], host->address.ip.v4.data[1],
           host->address.ip.v4.data[2], host->address.ip.v4.data[3],
           host->address.port, host->gamesRunning ? "true" : "false");
    fflush(stdout);

    if (probe->pairing_pin != NULL) {
        probe->authorization_requested = true;
        printf("authorizing device before streaming\n");
        fflush(stdout);
        if (!IHS_ClientAuthorizationRequest(client, host, probe->pairing_pin)) {
            fprintf(stderr, "authorization request could not be started\n");
            stop_client(client);
        }
        return;
    }
    request_stream(client, host, probe);
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
    (void) steam_id;
    ProbeContext *probe = context;
    probe->authorization_succeeded = true;
    printf("authorization success; requesting stream with the same client identity\n");
    fflush(stdout);
    request_stream(client, host, probe);
}

static void on_authorization_failed(IHS_Client *client, const IHS_HostInfo *host,
                                    IHS_AuthorizationResult result, void *context) {
    (void) host;
    ProbeContext *probe = context;
    printf("authorization failed result=%d\n", result);
    fflush(stdout);
    probe->authorization_succeeded = false;
    stop_client(client);
}

static void on_streaming_progress(IHS_Client *client, const IHS_HostInfo *host, void *context) {
    (void) client;
    (void) host;
    (void) context;
    printf("streaming request in progress\n");
    fflush(stdout);
}

static void on_streaming_success(IHS_Client *client, const IHS_HostInfo *host,
                                 const IHS_SocketAddress *address,
                                 const uint8_t *session_key, size_t session_key_len,
                                 void *context) {
    ProbeContext *probe = context;
    probe->session_info.address = *address;
    probe->session_info.sessionKeyLen = session_key_len;
    memcpy(probe->session_info.sessionKey, session_key, session_key_len);
    probe->session_info.steamId = host->clientId;
    probe->video_address = *address;
    probe->succeeded = true;
    printf("streaming request success address=%u.%u.%u.%u:%u key_len=%zu\n",
           address->ip.v4.data[0], address->ip.v4.data[1], address->ip.v4.data[2],
           address->ip.v4.data[3], address->port, session_key_len);
    fflush(stdout);
    stop_client(client);
}

static void on_streaming_failed(IHS_Client *client, const IHS_HostInfo *host,
                                IHS_StreamingResult result, void *context) {
    (void) host;
    ProbeContext *probe = context;
    printf("streaming request failed result=%d\n", result);
    fflush(stdout);
    probe->succeeded = false;
    stop_client(client);
}

static int spawn_ffplay(ProbeContext *probe, const IHS_StreamVideoConfig *config) {
    if (!probe->display) return -1;

    if (mkfifo(probe->video_path, 0600) != 0 && errno != EEXIST) {
        perror("mkfifo video");
        return -1;
    }
    const char *format = config->codec == IHS_StreamVideoCodecHEVC ? "hevc" : "h264";
    pid_t pid = fork();
    if (pid == 0) {
        execlp("ffplay", "ffplay", "-hide_banner", "-loglevel", "warning",
               "-fflags", "nobuffer", "-flags", "low_delay", "-framedrop",
               "-f", format, "-i", probe->video_path, (char *) NULL);
        _exit(127);
    }
    if (pid < 0) {
        perror("fork ffplay");
        return -1;
    }
    probe->ffplay_pid = pid;
    probe->video_fifo = open(probe->video_path, O_WRONLY);
    if (probe->video_fifo < 0) {
        perror("open video fifo");
        return -1;
    }
    printf("video display started codec=%s resolution=%ux%u fifo=%s\n",
           format, config->width, config->height, probe->video_path);
    fflush(stdout);
    return 0;
}

static int video_start(IHS_Session *session, const IHS_StreamVideoConfig *config, void *context) {
    (void) session;
    ProbeContext *probe = context;
    printf("video start codec=%d resolution=%ux%u codec_data=%zu\n",
           config->codec, config->width, config->height, config->codecDataLen);
    fflush(stdout);
    probe->video_file = fopen("/tmp/lunarnx-steamlink-video.es", "wb");
    if (probe->video_file == NULL) {
        perror("open video output");
        return -1;
    }
    if (config->codecDataLen != 0) {
        fwrite(config->codecData, 1, config->codecDataLen, probe->video_file);
    }
    if (probe->display && spawn_ffplay(probe, config) != 0) {
        fprintf(stderr, "video display unavailable; continuing with file capture\n");
        probe->display = false;
    }
    return 0;
}

static IHS_StreamVideoSubmitResult video_submit(IHS_Session *session, IHS_Buffer *data,
                                                 IHS_StreamVideoFrameFlag flags, void *context) {
    (void) session;
    (void) flags;
    ProbeContext *probe = context;
    const uint8_t *bytes = IHS_BufferPointer(data);
    size_t len = data->size;
    if (probe->video_file != NULL) fwrite(bytes, 1, len, probe->video_file);
    if (probe->video_fifo >= 0) {
        ssize_t written = write(probe->video_fifo, bytes, len);
        if (written < 0 && (errno == EPIPE || errno == EBADF)) {
            close(probe->video_fifo);
            probe->video_fifo = -1;
        }
    }
    probe->video_frames++;
    probe->video_bytes += len;
    if ((probe->video_frames % 60) == 0) {
        printf("video frames=%u bytes=%zu\n", probe->video_frames, probe->video_bytes);
        fflush(stdout);
    }
    return IHS_StreamVideoSubmitOK;
}

static void video_stop(IHS_Session *session, void *context) {
    (void) session;
    ProbeContext *probe = context;
    if (probe->video_file != NULL) {
        fclose(probe->video_file);
        probe->video_file = NULL;
    }
    if (probe->video_fifo >= 0) {
        close(probe->video_fifo);
        probe->video_fifo = -1;
    }
    printf("video stop frames=%u bytes=%zu\n", probe->video_frames, probe->video_bytes);
    fflush(stdout);
}

static int audio_start(IHS_Session *session, const IHS_StreamAudioConfig *config, void *context) {
    (void) session;
    ProbeContext *probe = context;
    printf("audio start codec=%d channels=%u frequency=%u codec_data=%zu\n",
           config->codec, config->channels, config->frequency, config->codecDataLen);
    fflush(stdout);
    probe->audio_file = fopen(probe->audio_path, "wb");
    if (probe->audio_file == NULL) {
        perror("open audio output");
        return -1;
    }
    return 0;
}

static int audio_submit(IHS_Session *session, IHS_Buffer *data, void *context) {
    (void) session;
    ProbeContext *probe = context;
    size_t len = data->size;
    if (probe->audio_file != NULL) fwrite(IHS_BufferPointer(data), 1, len, probe->audio_file);
    probe->audio_packets++;
    probe->audio_bytes += len;
    return 0;
}

static void audio_stop(IHS_Session *session, void *context) {
    (void) session;
    ProbeContext *probe = context;
    if (probe->audio_file != NULL) {
        fclose(probe->audio_file);
        probe->audio_file = NULL;
    }
    printf("audio stop packets=%u bytes=%zu\n", probe->audio_packets, probe->audio_bytes);
    fflush(stdout);
}

static void session_configuring(IHS_Session *session, IHS_SessionConfig *config, void *context) {
    (void) session;
    (void) context;
    config->enableAudio = true;
    config->enableHevc = false;
}

static void session_connected(IHS_Session *session, void *context) {
    (void) session;
    (void) context;
    printf("session connected\n");
    fflush(stdout);
}

static void session_disconnected(IHS_Session *session, void *context) {
    (void) session;
    (void) context;
    printf("session disconnected\n");
    fflush(stdout);
}

static void on_signal(int signal_number) {
    (void) signal_number;
    if (active_session != NULL) {
        IHS_SessionDisconnect(active_session);
    } else if (active_client != NULL) {
        IHS_ClientStop(active_client);
    }
}

static int run_stream_probe(ProbeContext *probe) {
    IHS_ClientDiscoveryCallbacks discovery = {.discovered = on_discovered};
    IHS_ClientAuthorizationCallbacks authorization = {
        .progress = on_authorization_progress,
        .success = on_authorization_success,
        .failed = on_authorization_failed,
    };
    IHS_ClientStreamingCallbacks streaming = {
        .progress = on_streaming_progress,
        .success = on_streaming_success,
        .failed = on_streaming_failed,
    };
    active_client = IHS_ClientCreate(&client_config);
    if (active_client == NULL) return -1;
    IHS_ClientSetLogFunction(active_client, log_print);
    IHS_ClientSetDiscoveryCallbacks(active_client, &discovery, probe);
    IHS_ClientSetAuthorizationCallbacks(active_client, &authorization, probe);
    IHS_ClientSetStreamingCallbacks(active_client, &streaming, probe);
    if (!IHS_ClientStartDiscovery(active_client, 1000)) {
        fprintf(stderr, "could not start discovery\n");
        stop_client(active_client);
    }
    IHS_ClientThreadedJoin(active_client);
    IHS_ClientDestroy(active_client);
    active_client = NULL;
    return probe->succeeded ? 0 : -1;
}

static void usage(const char *program) {
    fprintf(stderr, "usage: %s [--pin SECURITY_PIN] [--pair-code PAIRING_CODE] [--duration SECONDS] [--no-display] [--video-fifo PATH] [--audio PATH]\n", program);
}

int main(int argc, char **argv) {
    unsigned duration = 60;
    ProbeContext probe = {.video_fifo = -1, .ffplay_pid = -1,
                          .video_path = "/tmp/lunarnx-steamlink-video.fifo",
                          .audio_path = "/tmp/lunarnx-steamlink-audio.opus",
                          .display = true};
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
            probe.stream_pin = argv[++i];
        } else if (strcmp(argv[i], "--pair-code") == 0 && i + 1 < argc) {
            probe.pairing_pin = argv[++i];
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = (unsigned) strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--no-display") == 0) {
            probe.display = false;
        } else if (strcmp(argv[i], "--video-fifo") == 0 && i + 1 < argc) {
            probe.video_path = argv[++i];
        } else if (strcmp(argv[i], "--audio") == 0 && i + 1 < argc) {
            probe.audio_path = argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    IHS_Init();
    printf("requesting Steam Remote Play stream; duration=%us\n", duration);
    fflush(stdout);
    if (run_stream_probe(&probe) != 0) {
        fprintf(stderr, "Steam streaming request failed\n");
        IHS_Quit();
        return 1;
    }

    IHS_StreamSessionCallbacks session_callbacks = {
        .configuring = session_configuring,
        .connected = session_connected,
        .disconnected = session_disconnected,
    };
    IHS_StreamVideoCallbacks video_callbacks = {
        .start = video_start,
        .submit = video_submit,
        .stop = video_stop,
    };
    IHS_StreamAudioCallbacks audio_callbacks = {
        .start = audio_start,
        .submit = audio_submit,
        .stop = audio_stop,
    };
    IHS_Session *session = IHS_SessionCreate(&client_config, &probe.session_info);
    if (session == NULL) {
        IHS_Quit();
        return 1;
    }
    IHS_SessionSetLogFunction(session, log_print);
    IHS_SessionSetSessionCallbacks(session, &session_callbacks, &probe);
    IHS_SessionSetVideoCallbacks(session, &video_callbacks, &probe);
    IHS_SessionSetAudioCallbacks(session, &audio_callbacks, &probe);
    active_session = session;
    if (!IHS_SessionConnect(session)) {
        fprintf(stderr, "could not start Steam session\n");
        IHS_SessionDestroy(session);
        active_session = NULL;
        IHS_Quit();
        return 1;
    }
    alarm(duration);
    IHS_SessionThreadedJoin(session);
    active_session = NULL;
    IHS_SessionDestroy(session);
    if (probe.ffplay_pid > 0) {
        kill(probe.ffplay_pid, SIGTERM);
    }
    IHS_Quit();
    printf("summary video_frames=%u video_bytes=%zu audio_packets=%u audio_bytes=%zu\n",
           probe.video_frames, probe.video_bytes, probe.audio_packets, probe.audio_bytes);
    return probe.video_frames > 0 && probe.audio_packets > 0 ? 0 : 1;
}
