#include "platform/network_worker.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

namespace {

using namespace std::chrono_literals;

bool require(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

} // namespace

int main() {
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::atomic<bool> observed_shutdown{false};

    const bool started = lunar::platform::startNetworkWorker(
        "shutdown-test", [&]() {
            entered.set_value();
            while (!lunar::platform::networkWorkersShuttingDown()) {
                std::this_thread::sleep_for(1ms);
            }
            observed_shutdown = true;
        });

    bool ok = true;
    ok &= require(started, "worker should start before shutdown");
    if (!started) return 1;
    ok &= require(entered_future.wait_for(1s) == std::future_status::ready,
                  "worker should enter promptly");

    auto shutdown = std::async(std::launch::async, []() {
        lunar::platform::shutdownNetworkWorkers();
    });
    ok &= require(shutdown.wait_for(1s) == std::future_status::ready,
                  "shutdown should wait for a cooperative worker and complete");
    ok &= require(observed_shutdown.load(),
                  "running workers must observe the shutdown request");
    ok &= require(lunar::platform::activeNetworkWorkerCount() == 0,
                  "shutdown must drain every tracked worker");
    ok &= require(!lunar::platform::startNetworkWorker("too-late", []() {}),
                  "new workers must be rejected once shutdown begins");

    if (!ok) return 1;
    std::cout << "Network worker shutdown tests passed\n";
    return 0;
}
