#pragma once

#include <cstddef>
#include <functional>

namespace lunar::platform {

bool startNetworkWorker(const char* name,
                        std::function<void()> task,
                        size_t stack_size = 2 * 1024 * 1024);

} // namespace lunar::platform
