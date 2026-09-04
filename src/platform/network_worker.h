#pragma once

#include <cstddef>
#include <functional>

namespace lunar::platform {

bool startNetworkWorker(const char* name,
                        std::function<void()> task,
                        size_t stack_size = 2 * 1024 * 1024);

// Application shutdown is terminal. Running workers can observe the request and
// stop cooperatively; shutdownNetworkWorkers() then waits until every tracked
// worker has returned before Borealis destroys UI and platform state.
bool networkWorkersShuttingDown();
size_t activeNetworkWorkerCount();
void shutdownNetworkWorkers();

} // namespace lunar::platform
