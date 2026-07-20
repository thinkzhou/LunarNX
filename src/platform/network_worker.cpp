#include "network_worker.h"
#include "../diagnostics.h"

#include <exception>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>

#if defined(__SWITCH__)
#include <pthread.h>
#endif

namespace lunar::platform {
namespace {

struct NetworkWorkerTask {
    std::string name;
    std::function<void()> task;
};

void runNetworkWorker(NetworkWorkerTask* raw_task) {
    std::unique_ptr<NetworkWorkerTask> task(raw_task);
    const char* name = task->name.empty() ? "network" : task->name.c_str();
    lunar::diagnosticLog("network-worker", "%s entry begin", name);

    try {
        lunar::diagnosticLog("network-worker", "%s task begin", name);
        task->task();
        lunar::diagnosticLog("network-worker", "%s task done", name);
    } catch (const std::exception& e) {
        lunar::diagnosticLog("network-worker", "%s exception: %s", name, e.what());
    } catch (...) {
        lunar::diagnosticLog("network-worker", "%s unknown exception", name);
    }
}

#if defined(__SWITCH__)
void* networkWorkerEntry(void* arg) {
    lunar::diagnosticLog("network-worker", "pthread entry raw=%p", arg);
    runNetworkWorker(static_cast<NetworkWorkerTask*>(arg));
    return nullptr;
}
#endif

} // namespace

bool startNetworkWorker(const char* name, std::function<void()> task, size_t stack_size) {
    lunar::diagnosticLog("network-worker", "%s start requested stack=%zu",
                         name ? name : "network", stack_size);
    auto* heap_task = new (std::nothrow) NetworkWorkerTask{
        name ? name : "network",
        std::move(task),
    };
    if (!heap_task) {
        lunar::diagnosticLog("network-worker", "%s allocation failed",
                             name ? name : "network");
        return false;
    }

#if defined(__SWITCH__)
    pthread_attr_t attr;
    lunar::diagnosticLog("network-worker", "%s attr init begin",
                         name ? name : "network");
    int rc = pthread_attr_init(&attr);
    if (rc != 0) {
        delete heap_task;
        lunar::diagnosticLog("network-worker", "%s attr init failed rc=%d",
                             name ? name : "network", rc);
        return false;
    }

    lunar::diagnosticLog("network-worker", "%s attr stack begin",
                         name ? name : "network");
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, stack_size);

    pthread_t thread{};
    lunar::diagnosticLog("network-worker", "%s pthread create begin",
                         name ? name : "network");
    rc = pthread_create(&thread, &attr, networkWorkerEntry, heap_task);
    lunar::diagnosticLog("network-worker", "%s pthread create done rc=%d",
                         name ? name : "network", rc);
    lunar::diagnosticLog("network-worker", "%s attr destroy begin",
                         name ? name : "network");
    pthread_attr_destroy(&attr);
    lunar::diagnosticLog("network-worker", "%s attr destroy done",
                         name ? name : "network");
    if (rc != 0) {
        delete heap_task;
        lunar::diagnosticLog("network-worker", "%s create failed rc=%d",
                             name ? name : "network", rc);
        return false;
    }

    lunar::diagnosticLog("network-worker", "%s started stack=%zu",
                         name ? name : "network", stack_size);
    return true;
#else
    try {
        std::thread([heap_task]() { runNetworkWorker(heap_task); }).detach();
    } catch (const std::exception& e) {
        delete heap_task;
        lunar::diagnosticLog("network-worker", "%s create failed: %s",
                             name ? name : "network", e.what());
        return false;
    }

    lunar::diagnosticLog("network-worker", "%s started", name ? name : "network");
    return true;
#endif
}

} // namespace lunar::platform
