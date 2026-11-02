#pragma once
#include <functional>
#include <future>
#include <memory>
#include <tuple>
#include <utility>

// Compatibility entry point used by the MPL-licensed Horizon adaptations.
// Threads remain owned until joined; Horizon does not implement detach().
namespace lunar::webrtc {
std::shared_future<void> startRtcTask(std::function<void()> task);
void joinRtcTasks();
template<class F, class... Args>
std::shared_future<void> backgroundTask(F&& function, Args&&... args) {
    auto work = std::make_shared<std::packaged_task<void()>>(
        [fn = std::forward<F>(function), values = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            std::apply(std::move(fn), std::move(values));
        });
    return startRtcTask([work] { (*work)(); });
}
}
