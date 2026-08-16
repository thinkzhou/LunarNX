// SPDX-License-Identifier: AGPL-3.0-or-later

#include <chiaki/http.h>
#include <chiaki/log.h>
#include <chiaki/regist.h>
#include <chiaki/remote/rudp.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define REGIST_PORT 9295

typedef struct probe_server_t {
    int udp_sock;
    int tcp_sock;
    const char *label;
    const char *expected_search;
    const char *expected_path;
    bool search_ok;
    bool request_ok;
    char error[160];
} ProbeServer;

typedef struct probe_result_t {
    ChiakiRegistEventType event_type;
    bool received_event;
} ProbeResult;

static void set_error(ProbeServer *server, const char *message)
{
    if (!server->error[0])
        snprintf(server->error, sizeof(server->error), "%s", message);
}

static bool set_socket_timeout(int sock)
{
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0
        && setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}

static bool open_server_sockets(ProbeServer *server)
{
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(REGIST_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    server->udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    server->tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->udp_sock < 0 || server->tcp_sock < 0) {
        set_error(server, "socket creation failed");
        return false;
    }

    int reuse = 1;
    setsockopt(server->udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(server->tcp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (!set_socket_timeout(server->udp_sock) || !set_socket_timeout(server->tcp_sock)) {
        set_error(server, "socket timeout setup failed");
        return false;
    }
    if (bind(server->udp_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        bind(server->tcp_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(server->tcp_sock, 1) != 0) {
        set_error(server, "could not bind loopback registration port 9295");
        return false;
    }
    return true;
}

static void close_server_sockets(ProbeServer *server)
{
    if (server->udp_sock >= 0) close(server->udp_sock);
    if (server->tcp_sock >= 0) close(server->tcp_sock);
    server->udp_sock = -1;
    server->tcp_sock = -1;
}

static size_t expected_request_size(const char *request, size_t header_size)
{
    const char *content_length = strstr(request, "Content-Length:");
    if (!content_length) return 0;
    content_length += strlen("Content-Length:");
    return header_size + (size_t)strtoull(content_length, NULL, 10);
}

static void *server_thread(void *user)
{
    ProbeServer *server = user;
    struct sockaddr_storage peer = { 0 };
    socklen_t peer_size = sizeof(peer);
    char search[32] = { 0 };
    ssize_t search_size = recvfrom(server->udp_sock, search, sizeof(search), 0,
                                   (struct sockaddr *)&peer, &peer_size);
    if (search_size < 4) {
        set_error(server, "did not receive Chiaki search packet");
        return NULL;
    }
    server->search_ok = memcmp(search, server->expected_search, 4) == 0;
    if (!server->search_ok) {
        set_error(server, "received the wrong Chiaki search packet");
        return NULL;
    }

    const char *response = strcmp(server->expected_search, "SRC3") == 0
        ? "RES3" : "RES2";
    if (sendto(server->udp_sock, response, 4, 0,
               (struct sockaddr *)&peer, peer_size) != 4) {
        set_error(server, "could not send Chiaki search response");
        return NULL;
    }

    int client = accept(server->tcp_sock, NULL, NULL);
    if (client < 0) {
        set_error(server, "Chiaki did not open the registration TCP connection");
        return NULL;
    }
    set_socket_timeout(client);

    char request[2048] = { 0 };
    size_t filled = 0;
    size_t wanted = 0;
    while (filled < sizeof(request) - 1) {
        ssize_t received = recv(client, request + filled,
                                sizeof(request) - 1 - filled, 0);
        if (received <= 0) break;
        filled += (size_t)received;
        request[filled] = '\0';
        if (!wanted) {
            char *header_end = strstr(request, "\r\n\r\n");
            if (header_end) {
                size_t header_size = (size_t)(header_end + 4 - request);
                wanted = expected_request_size(request, header_size);
            }
        }
        if (wanted && filled >= wanted) break;
    }

    server->request_ok = strstr(request, server->expected_path) != NULL;
    if (!server->request_ok) {
        set_error(server, "registration request used the wrong endpoint");
    }

    static const char reject[] =
        "HTTP/1.1 403 Forbidden\r\n"
        "RP-Application-Reason: 80108b09\r\n"
        "Content-Length: 0\r\n\r\n";
    send(client, reject, sizeof(reject) - 1, 0);
    shutdown(client, SHUT_RDWR);
    close(client);
    return NULL;
}

static void regist_event(ChiakiRegistEvent *event, void *user)
{
    ProbeResult *result = user;
    if (!event) return;
    result->event_type = event->type;
    result->received_event = true;
}

static void quiet_log(ChiakiLogLevel level, const char *message, void *user)
{
    (void)level;
    (void)message;
    (void)user;
}

static bool run_probe(const char *label, ChiakiTarget target,
                      const char *search, const char *path)
{
    ProbeServer server = {
        .udp_sock = -1,
        .tcp_sock = -1,
        .label = label,
        .expected_search = search,
        .expected_path = path,
    };
    if (!open_server_sockets(&server)) {
        fprintf(stderr, "%s probe setup failed: %s (%s)\n",
                label, server.error, strerror(errno));
        close_server_sockets(&server);
        return false;
    }

    pthread_t server_worker;
    if (pthread_create(&server_worker, NULL, server_thread, &server) != 0) {
        fprintf(stderr, "%s probe could not start fake host\n", label);
        close_server_sockets(&server);
        return false;
    }

    ChiakiLog log;
    chiaki_log_init(&log, CHIAKI_LOG_ALL, quiet_log, NULL);
    ChiakiRegistInfo info = { 0 };
    info.target = target;
    info.host = "127.0.0.1";
    info.broadcast = false;
    info.pin = 12345678;
    for (size_t i = 0; i < sizeof(info.psn_account_id); ++i)
        info.psn_account_id[i] = (uint8_t)(i + 1);

    ProbeResult result = { 0 };
    ChiakiRegist regist = { 0 };
    ChiakiErrorCode start_error = chiaki_regist_start(
        &regist, &log, &info, regist_event, &result);
    if (start_error == CHIAKI_ERR_SUCCESS)
        chiaki_regist_fini(&regist);
    else
        set_error(&server, "chiaki_regist_start failed");

    pthread_join(server_worker, NULL);
    close_server_sockets(&server);

    const bool passed = start_error == CHIAKI_ERR_SUCCESS &&
        result.received_event &&
        result.event_type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_FAILED &&
        server.search_ok && server.request_ok && !server.error[0];
    printf("%s search=%s request=%s result=%s\n",
           label, search, path, passed ? "PASS" : "FAIL");
    if (!passed && server.error[0])
        fprintf(stderr, "%s probe: %s\n", label, server.error);
    return passed;
}

/* LAN-only probe: keep the link independent from Chiaki's PSN RUDP objects. */
ChiakiErrorCode chiaki_rudp_send_recv(ChiakiRudp rudp, RudpMessage *message,
    uint8_t *buf, size_t buf_size, uint16_t remote_counter,
    RudpPacketType send_type, RudpPacketType recv_type,
    size_t min_data_size, size_t tries)
{
    (void)rudp; (void)message; (void)buf; (void)buf_size;
    (void)remote_counter; (void)send_type; (void)recv_type;
    (void)min_data_size; (void)tries;
    return CHIAKI_ERR_UNKNOWN;
}

void chiaki_rudp_message_pointers_free(RudpMessage *message)
{
    (void)message;
}

const char *chiaki_rp_application_reason_string(uint32_t reason)
{
    (void)reason;
    return "registration rejected";
}

const char *chiaki_error_string(ChiakiErrorCode error)
{
    (void)error;
    return "probe error";
}

const char *chiaki_rp_version_string(ChiakiTarget target)
{
    switch (target) {
        case CHIAKI_TARGET_PS4_9: return "9.0";
        case CHIAKI_TARGET_PS4_10: return "10.0";
        case CHIAKI_TARGET_PS5_1: return "1.0";
        default: return NULL;
    }
}

int main(void)
{
    bool ok = true;
    ok &= run_probe("PS4", CHIAKI_TARGET_PS4_10, "SRC2",
                    "/sie/ps4/rp/sess/rgst");
    ok &= run_probe("PS5", CHIAKI_TARGET_PS5_1, "SRC3",
                    "/sie/ps5/rp/sess/rgst");
    return ok ? 0 : 1;
}
