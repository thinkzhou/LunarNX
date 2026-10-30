#include "background.hpp"
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace lunar::webrtc {
namespace {
struct Task { std::thread thread; std::shared_future<void> done; };
struct Tasks { std::mutex mutex; std::vector<Task> running; };
Tasks& tasks() { static Tasks value; return value; }
}
std::shared_future<void> startRtcTask(std::function<void()> work) {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future().share();
    auto& owner = tasks();
    std::lock_guard<std::mutex> guard(owner.mutex);
    for (auto it = owner.running.begin(); it != owner.running.end();) {
        if (it->thread.get_id() != std::this_thread::get_id() &&
            it->done.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            it->thread.join(); it = owner.running.erase(it);
        } else ++it;
    }
    // Reserve before creating a thread: a vector allocation failure must not
    // destroy a still-joinable temporary std::thread.
    owner.running.reserve(owner.running.size() + 1);
    std::thread thread([work = std::move(work), promise] {
        try { work(); promise->set_value(); }
        catch (...) { promise->set_exception(std::current_exception()); }
    });
    owner.running.push_back({std::move(thread), future});
    return future;
}
void joinRtcTasks() {
    auto& owner = tasks();
    for (;;) {
        std::vector<Task> batch;
        { std::lock_guard<std::mutex> guard(owner.mutex); batch.swap(owner.running); }
        if (batch.empty()) return;
        for (auto& task : batch) if (task.thread.joinable()) task.thread.join();
    }
}
}
