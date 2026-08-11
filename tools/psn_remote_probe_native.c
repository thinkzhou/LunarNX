// SPDX-License-Identifier: AGPL-3.0-only

#include <arpa/inet.h>

#include <chiaki/base64.h>
#include <chiaki/log.h>
#include <chiaki/remote/holepunch.h>
#include <chiaki/session.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#include <json-c/json.h>
extern void lunarnx_chiaki_set_ryubing_compat(bool enabled);
#endif

typedef struct probe_state_t {
    atomic_bool connected;
    atomic_bool data_hole_started;
    atomic_bool data_hole_finished;
    atomic_bool registered;
    atomic_bool quit;
    atomic_bool login_pin_requested;
    atomic_uint_fast64_t video_frames;
    atomic_uint_fast64_t video_bytes;
    atomic_uint_fast64_t audio_frames;
    atomic_uint_fast64_t audio_bytes;
    atomic_uint_fast64_t frames_lost;
    ChiakiQuitReason quit_reason;
} ProbeState;

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void trace_phase(const char *phase, const char *status, double elapsed_ms)
{
    printf("TRACE {\"phase\":\"%s\",\"status\":\"%s\",\"elapsed_ms\":%.1f}\n",
        phase, status, elapsed_ms);
    fflush(stdout);
}

static void probe_log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
    (void)user;
    printf("CHIAKI %c %s\n", chiaki_log_level_char(level), msg);
    fflush(stdout);
}

static bool video_sample_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost,
    bool frame_recovered, void *user)
{
    (void)buf;
    (void)frame_recovered;
    ProbeState *state = user;
    uint64_t previous = atomic_fetch_add(&state->video_frames, 1);
    atomic_fetch_add(&state->video_bytes, buf_size);
    if(frames_lost > 0)
        atomic_fetch_add(&state->frames_lost, (uint64_t)frames_lost);
    if(previous == 0)
        trace_phase("first_h264_sample", "ok", 0.0);
    return true;
}

static void audio_header_cb(ChiakiAudioHeader *header, void *user)
{
    (void)user;
    printf("TRACE {\"phase\":\"opus_stream_info\",\"status\":\"ok\","
        "\"channels\":%u,\"rate\":%u,\"frame_size\":%u}\n",
        header->channels, header->rate, header->frame_size);
    fflush(stdout);
}

static void audio_frame_cb(uint8_t *buf, size_t buf_size, void *user)
{
    (void)buf;
    ProbeState *state = user;
    uint64_t previous = atomic_fetch_add(&state->audio_frames, 1);
    atomic_fetch_add(&state->audio_bytes, buf_size);
    if(previous == 0)
        trace_phase("first_opus_packet", "ok", 0.0);
}

static void session_event_cb(ChiakiEvent *event, void *user)
{
    ProbeState *state = user;
    switch(event->type)
    {
        case CHIAKI_EVENT_CONNECTED:
            atomic_store(&state->connected, true);
            trace_phase("session_connected", "ok", 0.0);
            break;
        case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
            atomic_store(&state->login_pin_requested, true);
            trace_phase("login_pin", event->login_pin_request.pin_incorrect
                ? "incorrect" : "required", 0.0);
            break;
        case CHIAKI_EVENT_HOLEPUNCH:
            if(event->data_holepunch.finished)
            {
                atomic_store(&state->data_hole_finished, true);
                trace_phase("data_hole_punch", "ok", 0.0);
            }
            else
            {
                atomic_store(&state->data_hole_started, true);
                trace_phase("data_hole_punch", "started", 0.0);
            }
            break;
        case CHIAKI_EVENT_REGIST:
            atomic_store(&state->registered, true);
            trace_phase("dynamic_registration", "ok", 0.0);
            break;
        case CHIAKI_EVENT_QUIT:
            state->quit_reason = event->quit.reason;
            atomic_store(&state->quit, true);
            printf("TRACE {\"phase\":\"session_quit\",\"status\":\"%s\","
                "\"reason\":\"%s\"}\n",
                chiaki_quit_reason_is_error(event->quit.reason) ? "error" : "ok",
                chiaki_quit_reason_string(event->quit.reason));
            fflush(stdout);
            break;
        default:
            break;
    }
}

static int fail_phase(const char *phase, ChiakiErrorCode err, double started_at)
{
    printf("TRACE {\"phase\":\"%s\",\"status\":\"error\","
        "\"error\":\"%s\",\"elapsed_ms\":%.1f}\n",
        phase, chiaki_error_string(err),
        (monotonic_seconds() - started_at) * 1000.0);
    fflush(stdout);
    return 1;
}

static int run_probe(const char *device_name, int media_seconds,
    const char *token, const char *account_id, const char *login_pin,
    const char *relay_host, const char *relay_port_text, const char *relay_secret)
{
#ifdef __SWITCH__
    lunarnx_chiaki_set_ryubing_compat(true);
#endif
    if(!token || !*token || !account_id || !*account_id)
    {
        fprintf(stderr, "PSN token or account ID environment is missing\n");
        return 2;
    }
    if(media_seconds < 1 || media_seconds > 120)
    {
        fprintf(stderr, "media-seconds must be between 1 and 120\n");
        return 2;
    }

    ChiakiLog log;
    /* Verbose RUDP hexdumps are useful on desktop, but they can starve the
     * emulated Switch worker and change the PS5 hole-punch timing. Keep the
     * automated Ryubing probe focused on state transitions and errors. */
#ifdef __SWITCH__
    chiaki_log_init(&log, CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR,
        probe_log_cb, NULL);
#else
    chiaki_log_init(&log, CHIAKI_LOG_ALL, probe_log_cb, NULL);
#endif
    ChiakiHolepunchDeviceInfo *devices = NULL;
    size_t device_count = 0;
    double started_at = monotonic_seconds();
    ChiakiErrorCode err = chiaki_holepunch_list_devices(token,
        CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5, &devices, &device_count, &log);
    if(err != CHIAKI_ERR_SUCCESS)
        return fail_phase("list_devices", err, started_at);
    trace_phase("list_devices", "ok", (monotonic_seconds() - started_at) * 1000.0);

    ChiakiHolepunchDeviceInfo *selected = NULL;
    for(size_t i = 0; i < device_count; ++i)
    {
        if(strcmp(devices[i].device_name, device_name) == 0)
        {
            selected = &devices[i];
            break;
        }
    }
    if(!selected)
    {
        printf("TRACE {\"phase\":\"select_device\",\"status\":\"not_found\","
            "\"available_count\":%zu}\n", device_count);
        chiaki_holepunch_free_device_list(&devices);
        return 1;
    }
    trace_phase("select_device", "ok", 0.0);

    ChiakiHolepunchSession holepunch = chiaki_holepunch_session_init(token, &log);
    if(!holepunch)
    {
        chiaki_holepunch_free_device_list(&devices);
        trace_phase("holepunch_init", "error", 0.0);
        return 1;
    }
#ifdef __SWITCH__
    chiaki_holepunch_session_set_port_guessing_socks(holepunch, 64);
#endif

    if(relay_host || relay_port_text || relay_secret)
    {
        char *port_end = NULL;
        long relay_port = relay_port_text
            ? strtol(relay_port_text, &port_end, 10) : 0;
        if(!relay_host || !*relay_host || !relay_port_text || !relay_secret
            || !*relay_secret || !port_end || *port_end != '\0'
            || relay_port < 1 || relay_port > UINT16_MAX)
        {
            trace_phase("relay_config", "error", 0.0);
            goto fail_holepunch;
        }
        err = chiaki_holepunch_session_set_relay(holepunch, relay_host,
            (uint16_t)relay_port, relay_secret);
        if(err != CHIAKI_ERR_SUCCESS)
        {
            fail_phase("relay_config", err, monotonic_seconds());
            goto fail_holepunch;
        }
        trace_phase("relay_config", "ok", 0.0);
    }

    started_at = monotonic_seconds();
#ifdef __SWITCH__
    trace_phase("upnp_discovery", "skipped", 0.0);
#else
    err = chiaki_holepunch_upnp_discover(holepunch);
    if(err != CHIAKI_ERR_SUCCESS)
        trace_phase("upnp_discovery", "unavailable", (monotonic_seconds() - started_at) * 1000.0);
    else
        trace_phase("upnp_discovery", "ok", (monotonic_seconds() - started_at) * 1000.0);
#endif

    started_at = monotonic_seconds();
    err = chiaki_holepunch_session_create(holepunch);
    if(err != CHIAKI_ERR_SUCCESS)
    {
        fail_phase("session_create", err, started_at);
        goto fail_holepunch;
    }
    trace_phase("session_create", "ok", (monotonic_seconds() - started_at) * 1000.0);

    started_at = monotonic_seconds();
    err = holepunch_session_create_offer(holepunch);
    if(err != CHIAKI_ERR_SUCCESS)
    {
        fail_phase("control_offer", err, started_at);
        goto fail_holepunch;
    }
    trace_phase("control_offer", "ok", (monotonic_seconds() - started_at) * 1000.0);

    started_at = monotonic_seconds();
    err = chiaki_holepunch_session_start(holepunch, selected->device_uid,
        CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5);
    if(err != CHIAKI_ERR_SUCCESS)
    {
        fail_phase("console_start", err, started_at);
        goto fail_holepunch;
    }
    trace_phase("console_start", "ok", (monotonic_seconds() - started_at) * 1000.0);

    started_at = monotonic_seconds();
    err = chiaki_holepunch_session_punch_hole(holepunch,
        CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL);
    if(err != CHIAKI_ERR_SUCCESS)
    {
        fail_phase("control_hole_punch", err, started_at);
        goto fail_holepunch;
    }
    trace_phase("control_hole_punch", "ok", (monotonic_seconds() - started_at) * 1000.0);

    ChiakiConnectInfo connect_info = {0};
    connect_info.ps5 = true;
    connect_info.host = NULL;
    chiaki_connect_video_profile_preset(&connect_info.video_profile,
        CHIAKI_VIDEO_RESOLUTION_PRESET_720p, CHIAKI_VIDEO_FPS_PRESET_60);
    connect_info.video_profile_auto_downgrade = true;
    connect_info.holepunch_session = holepunch;
    connect_info.packet_loss_max = 0.02;
    connect_info.enable_idr_on_fec_failure = true;
    size_t account_id_size = sizeof(connect_info.psn_account_id);
    err = chiaki_base64_decode(account_id, strlen(account_id),
        connect_info.psn_account_id, &account_id_size);
    if(err != CHIAKI_ERR_SUCCESS || account_id_size != sizeof(connect_info.psn_account_id))
    {
        trace_phase("account_id_decode", "error", 0.0);
        goto fail_holepunch;
    }

    ProbeState state = {0};
    ChiakiSession session;
    err = chiaki_session_init(&session, &connect_info, &log);
    if(err != CHIAKI_ERR_SUCCESS)
    {
        fail_phase("stream_session_init", err, monotonic_seconds());
        goto fail_holepunch;
    }
    holepunch = NULL; // ChiakiSession owns it after successful init.
    chiaki_session_set_event_cb(&session, session_event_cb, &state);
    chiaki_session_set_video_sample_cb(&session, video_sample_cb, &state);
    ChiakiAudioSink audio_sink = {
        .user = &state,
        .header_cb = audio_header_cb,
        .frame_cb = audio_frame_cb,
    };
    chiaki_session_set_audio_sink(&session, &audio_sink);

    started_at = monotonic_seconds();
    err = chiaki_session_start(&session);
    if(err != CHIAKI_ERR_SUCCESS)
    {
        fail_phase("stream_session_start", err, started_at);
        chiaki_session_fini(&session);
        goto fail_devices;
    }
    trace_phase("stream_session_start", "ok", (monotonic_seconds() - started_at) * 1000.0);

    double deadline = monotonic_seconds() + 90.0;
    double media_deadline = 0.0;
    bool pin_submitted = false;
    while(monotonic_seconds() < deadline && !atomic_load(&state.quit))
    {
        if(atomic_load(&state.login_pin_requested) && !pin_submitted && login_pin && *login_pin)
        {
            chiaki_session_set_login_pin(&session,
                (const uint8_t *)login_pin, strlen(login_pin));
            pin_submitted = true;
            trace_phase("login_pin_submit", "ok", 0.0);
        }
        if(atomic_load(&state.video_frames) > 0 && atomic_load(&state.audio_frames) > 0)
        {
            if(media_deadline == 0.0)
                media_deadline = monotonic_seconds() + media_seconds;
            if(monotonic_seconds() >= media_deadline)
                break;
        }
        usleep(100000);
    }

    started_at = monotonic_seconds();
    trace_phase("stream_stop", "started", 0.0);
    err = chiaki_session_stop(&session);
    if(err != CHIAKI_ERR_SUCCESS)
        fail_phase("stream_stop", err, started_at);
    else
        trace_phase("stream_stop", "ok", (monotonic_seconds() - started_at) * 1000.0);

    started_at = monotonic_seconds();
    err = chiaki_session_join(&session);
    if(err != CHIAKI_ERR_SUCCESS)
        fail_phase("stream_join", err, started_at);
    else
        trace_phase("stream_join", "ok", (monotonic_seconds() - started_at) * 1000.0);
    atomic_store(&state.registered, session.psn_regist_succeeded);
    bool clean_stop = atomic_load(&state.quit)
        && state.quit_reason == CHIAKI_QUIT_REASON_STOPPED;

    started_at = monotonic_seconds();
    chiaki_session_fini(&session);
    trace_phase("psn_session_cleanup", "ok",
        (monotonic_seconds() - started_at) * 1000.0);
    printf("TRACE {\"phase\":\"media_summary\",\"status\":\"%s\","
        "\"connected\":%s,\"registered\":%s,\"data_hole\":%s,\"clean_stop\":%s,"
        "\"video_frames\":%llu,\"video_bytes\":%llu,"
        "\"audio_frames\":%llu,\"audio_bytes\":%llu,"
        "\"frames_lost\":%llu}\n",
        atomic_load(&state.video_frames) > 0 && atomic_load(&state.audio_frames) > 0
            ? "ok" : "incomplete",
        atomic_load(&state.connected) ? "true" : "false",
        atomic_load(&state.registered) ? "true" : "false",
        atomic_load(&state.data_hole_finished) ? "true" : "false",
        clean_stop ? "true" : "false",
        (unsigned long long)atomic_load(&state.video_frames),
        (unsigned long long)atomic_load(&state.video_bytes),
        (unsigned long long)atomic_load(&state.audio_frames),
        (unsigned long long)atomic_load(&state.audio_bytes),
        (unsigned long long)atomic_load(&state.frames_lost));
    chiaki_holepunch_free_device_list(&devices);
    return atomic_load(&state.video_frames) > 0
        && atomic_load(&state.audio_frames) > 0 && clean_stop ? 0 : 1;

fail_holepunch:
    chiaki_holepunch_session_fini(holepunch);
fail_devices:
    chiaki_holepunch_free_device_list(&devices);
    return 1;
}

#ifdef __SWITCH__
static bool switch_read_file(const char *path, char **content)
{
    FILE *file = fopen(path, "r");
    if(!file) return false;
    if(fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long length = ftell(file);
    if(length <= 0 || length > 65536 || fseek(file, 0, SEEK_SET) != 0)
    { fclose(file); return false; }
    char *value = calloc((size_t)length + 1, 1);
    if(!value) { fclose(file); return false; }
    bool success = fread(value, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if(!success) { free(value); return false; }
    *content = value;
    return true;
}

static const char *switch_json_string(struct json_object *root, const char *key)
{
    struct json_object *value = NULL;
    if(!json_object_object_get_ex(root, key, &value)
        || !json_object_is_type(value, json_type_string))
        return NULL;
    return json_object_get_string(value);
}

static bool switch_load_config(char **token, char **account_id,
    char **relay_host, char **relay_port, char **relay_secret,
    char **device_name)
{
    char *token_text = NULL;
    char *config_text = NULL;
    struct json_object *token_root = NULL;
    struct json_object *config_root = NULL;
    bool success = false;
    if(!switch_read_file("sdmc:/switch/LunarNX/psn_token.json", &token_text)
        || !switch_read_file("sdmc:/switch/LunarNX/config.json", &config_text))
        goto cleanup;
    token_root = json_tokener_parse(token_text);
    config_root = json_tokener_parse(config_text);
    if(!token_root || !config_root) goto cleanup;

    const char *profile = switch_json_string(config_root, "ps_network_profile");
    const char *access = switch_json_string(token_root, "access_token");
    const char *account = switch_json_string(token_root, "account_id");
    const char *host = switch_json_string(config_root, "ps_relay_host");
    const char *secret = switch_json_string(config_root, "ps_relay_secret");
    struct json_object *port_value = NULL;
    if(!profile || strcmp(profile, "ryubing") != 0 || !access || !account
        || !host || !secret
        || !json_object_object_get_ex(config_root, "ps_relay_port", &port_value)
        || !json_object_is_type(port_value, json_type_int))
        goto cleanup;

    char port_buffer[16];
    snprintf(port_buffer, sizeof(port_buffer), "%d", json_object_get_int(port_value));
    const char *configured_device = switch_json_string(config_root, "ps_probe_device");
    if(!configured_device || !*configured_device) configured_device = "PS5-231";
    *token = strdup(access);
    *account_id = strdup(account);
    *relay_host = strdup(host);
    *relay_port = strdup(port_buffer);
    *relay_secret = strdup(secret);
    *device_name = strdup(configured_device);
    success = *token && *account_id && *relay_host && *relay_port
        && *relay_secret && *device_name;

cleanup:
    if(token_root) json_object_put(token_root);
    if(config_root) json_object_put(config_root);
    free(token_text);
    free(config_text);
    if(!success)
    {
        free(*token); free(*account_id); free(*relay_host); free(*relay_port);
        free(*relay_secret); free(*device_name);
        *token = NULL; *account_id = NULL; *relay_host = NULL;
        *relay_port = NULL; *relay_secret = NULL; *device_name = NULL;
    }
    return success;
}

static volatile int switch_probe_result = 2;

static void switch_probe_worker(void *arg)
{
    char **values = arg;
    switch_probe_result = run_probe(values[4], 2, values[0], values[1], NULL,
        values[2], values[3], values[5]);
    printf("PROBE_RESULT %s\n", switch_probe_result == 0 ? "PASS" : "FAIL");
    fflush(stdout);
}

int main(void)
{
    consoleInit(NULL);
    freopen("sdmc:/switch/LunarNX/ps_session_probe.log", "w", stdout);
    setvbuf(stdout, NULL, _IOLBF, 0);
    Result socket_result = socketInitializeDefault();
    if(R_FAILED(socket_result))
    {
        printf("PROBE_RESULT FAIL socketInitialize=0x%08x\n", socket_result);
        consoleExit(NULL);
        return 1;
    }

    char *values[6] = {NULL, NULL, NULL, NULL, NULL, NULL};
    if(!switch_load_config(&values[0], &values[1], &values[2], &values[3],
        &values[5], &values[4]))
    {
        printf("PROBE_RESULT FAIL config\n");
        socketExit();
        consoleExit(NULL);
        return 1;
    }

    const size_t stack_size = 8 * 1024 * 1024;
    void *stack_mem = aligned_alloc(0x1000, stack_size);
    Thread thread;
    Result thread_result = stack_mem
        ? threadCreate(&thread, switch_probe_worker, values,
            stack_mem, stack_size, 0x30, -2) : 1;
    if(R_FAILED(thread_result))
    {
        printf("PROBE_RESULT FAIL thread=0x%08x\n", thread_result);
        free(stack_mem);
        for(size_t i = 0; i < 6; ++i) free(values[i]);
        socketExit();
        consoleExit(NULL);
        return 1;
    }
    threadStart(&thread);
    while(appletMainLoop() && switch_probe_result == 2)
    {
        consoleUpdate(NULL);
        svcSleepThread(50 * 1000 * 1000LL);
    }
    threadWaitForExit(&thread);
    threadClose(&thread);
    free(stack_mem);
    for(size_t i = 0; i < 6; ++i) free(values[i]);
    socketExit();
    consoleExit(NULL);
    return switch_probe_result == 0 ? 0 : 1;
}
#else
int main(int argc, char **argv)
{
    if(argc < 2 || argc > 3)
    {
        fprintf(stderr, "Usage: %s <device-name> [media-seconds]\n", argv[0]);
        return 2;
    }
    return run_probe(argv[1], argc == 3 ? atoi(argv[2]) : 15,
        getenv("LUNARNX_PSN_ACCESS_TOKEN"), getenv("LUNARNX_PSN_ACCOUNT_ID"),
        getenv("LUNARNX_PSN_LOGIN_PIN"), getenv("LUNARNX_PS_RELAY_HOST"),
        getenv("LUNARNX_PS_RELAY_PORT"), getenv("LUNARNX_PS_RELAY_SECRET"));
}
#endif
