#include "api/http_client.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <future>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

bool require(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (!require(listener >= 0, "create loopback listener")) return 1;

    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (!require(bind(listener, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)) == 0,
                 "bind loopback listener") ||
        !require(listen(listener, 1) == 0, "listen on loopback socket")) {
        close(listener);
        return 1;
    }

    socklen_t address_length = sizeof(address);
    if (!require(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                             &address_length) == 0,
                 "read loopback listener port")) {
        close(listener);
        return 1;
    }

    std::promise<void> first_request_started;
    auto first_request_ready = first_request_started.get_future();
    std::thread server([listener, started = std::move(first_request_started)]() mutable {
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            started.set_value();
            close(listener);
            return;
        }

        char request[2048]{};
        recv(client, request, sizeof(request), 0);
        started.set_value();

        std::this_thread::sleep_for(1200ms);
        constexpr char response[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 2\r\n"
            "Connection: close\r\n"
            "\r\n"
            "ok";
        send(client, response, std::strlen(response), 0);
        close(client);
        close(listener);
    });

    lunar::api::HttpClient http;
    const std::string url = "http://127.0.0.1:" +
        std::to_string(ntohs(address.sin_port)) + "/slow";
    lunar::api::HttpResponse first_response;
    std::thread first_request([&]() { first_response = http.get(url); });
    first_request_ready.wait();

    std::atomic<bool> cancel_requested{false};
    std::thread cancel([&]() {
        std::this_thread::sleep_for(50ms);
        cancel_requested = true;
    });

    const auto started_at = std::chrono::steady_clock::now();
    const auto cancelled_response = http.get(
        url, {}, [&]() { return cancel_requested.load(); });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);

    cancel.join();
    first_request.join();
    server.join();

    bool ok = true;
    ok &= require(first_response.status_code == 200,
                  "the request holding the client lock should complete");
    ok &= require(cancelled_response.network_error,
                  "cancelled lock wait should report a network cancellation");
    ok &= require(cancelled_response.error_message == "cancelled",
                  "cancelled lock wait should have a stable error reason");
    ok &= require(elapsed < 400ms,
                  "cancelled request must not wait for the active HTTP request");
    if (!ok) {
        std::cerr << "cancel elapsed_ms=" << elapsed.count() << '\n';
        return 1;
    }

    std::cout << "HTTP client cancellation tests passed elapsed_ms="
              << elapsed.count() << '\n';
    return 0;
}
