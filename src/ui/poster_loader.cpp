#include "poster_loader.h"

#include "../api/http_client.h"
#include "../diagnostics.h"
#include "../platform/network_worker.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#ifdef __SWITCH__
#include <borealis/core/thread.hpp>
#endif

namespace lunar::ui {
namespace {

struct PosterJob {
    brls::Image* image = nullptr;
    std::string url;
    uint32_t seq = 0;
    PosterLoader::BatchId batch = 0;
};

std::mutex g_mutex;
std::deque<PosterJob> g_queue;
bool g_running = false;
std::atomic<uint32_t> g_seq{0};
PosterLoader::BatchId g_batch = 0;

std::string posterThumbnailUrl(const std::string& url) {
    if (url.find("store-images.s-microsoft.com") == std::string::npos) {
        return url;
    }
    const char separator = url.find('?') == std::string::npos ? '?' : '&';
    return url + separator + "w=240&h=360&q=80&format=jpg";
}

void startNextLocked(); // expects g_mutex held? no
void pumpQueue();

bool isBatchCurrent(PosterLoader::BatchId batch) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return batch != 0 && batch == g_batch;
}

void unlockJob(PosterJob& job) {
    if (!job.image) return;
    job.image->ptrUnlock();
    job.image = nullptr;
}

void finishAndContinue() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_running = false;
    }
    pumpQueue();
}

void pumpQueue() {
    PosterJob job;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_running) return;
        if (g_queue.empty()) return;
        job = std::move(g_queue.front());
        g_queue.pop_front();
        g_running = true;
    }

    if (!job.image || job.url.empty()) {
        finishAndContinue();
        return;
    }

    auto* image = job.image;
    const std::string url = posterThumbnailUrl(job.url);
    const uint32_t seq = job.seq;
    const auto batch = job.batch;

    lunar::diagnosticLog("poster", "start seq=%u batch=%u url=%s",
                         seq, batch, url.c_str());
    image->setFreeTexture(true);

    bool started = lunar::platform::startNetworkWorker(
        "poster-load",
        [image, url, seq, batch]() {
            lunar::diagnosticLog("poster", "download begin seq=%u", seq);
            lunar::api::HttpClient http;
            auto resp = http.get(url, {{"Accept", "image/*"}});
            if (resp.status_code != 200 || resp.body.empty()) {
                lunar::diagnosticLog("poster", "download fail seq=%u status=%d",
                                     seq, resp.status_code);
                brls::sync([image]() {
                    if (image) image->ptrUnlock();
                    finishAndContinue();
                });
                return;
            }
            if (resp.body.size() > 256 * 1024) {
                lunar::diagnosticLog("poster", "download too large seq=%u bytes=%zu",
                                     seq, resp.body.size());
                brls::sync([image]() {
                    if (image) image->ptrUnlock();
                    finishAndContinue();
                });
                return;
            }

            auto body = std::make_shared<std::string>(std::move(resp.body));
            lunar::diagnosticLog("poster", "download ok seq=%u bytes=%zu", seq, body->size());
            brls::sync([image, body, seq, batch]() {
                if (image) {
                    if (isBatchCurrent(batch)) {
                        image->setFreeTexture(true);
                        image->setImageFromMem(
                            reinterpret_cast<const unsigned char*>(body->data()),
                            static_cast<int>(body->size()));
                        lunar::diagnosticLog("poster", "apply texture seq=%u tex=%d",
                                             seq, image->getTexture());
                    } else {
                        lunar::diagnosticLog("poster", "skip stale seq=%u batch=%u",
                                             seq, batch);
                    }
                    image->ptrUnlock();
                }
                finishAndContinue();
            });
        },
        1 * 1024 * 1024);

    if (!started) {
        lunar::diagnosticLog("poster", "worker start failed seq=%u", seq);
        image->ptrUnlock();
        finishAndContinue();
    }
}

} // namespace

PosterLoader& PosterLoader::instance() {
    static PosterLoader loader;
    return loader;
}

#ifdef __SWITCH__

PosterLoader::BatchId PosterLoader::beginBatch() {
    std::deque<PosterJob> cancelled;
    BatchId batch = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        batch = ++g_batch;
        if (batch == 0) batch = ++g_batch;
        cancelled.swap(g_queue);
    }

    for (auto& job : cancelled) {
        lunar::diagnosticLog("poster", "cancel queued seq=%u batch=%u",
                             job.seq, job.batch);
        job.image->ptrUnlock();
        job.image = nullptr;
    }
    lunar::diagnosticLog("poster", "begin batch=%u cancelled=%zu",
                         batch, cancelled.size());
    return batch;
}

void PosterLoader::load(brls::Image* view, const std::string& url, BatchId batch) {
    if (!view || url.empty()) return;
    const uint32_t seq = ++g_seq;
    PosterJob dropped;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (batch == 0 || batch != g_batch) {
            lunar::diagnosticLog("poster", "ignore stale enqueue seq=%u batch=%u current=%u",
                                 seq, batch, g_batch);
            return;
        }

        view->ptrLock();
        // Bound queue.
        if (g_queue.size() >= 32) {
            dropped = std::move(g_queue.front());
            g_queue.pop_front();
            lunar::diagnosticLog("poster", "drop queued seq=%u", dropped.seq);
        }
        g_queue.push_back(PosterJob{view, url, seq, batch});
        lunar::diagnosticLog("poster", "enqueue seq=%u batch=%u q=%zu url=%s",
                             seq, batch, g_queue.size(), url.c_str());
    }
    unlockJob(dropped);
    pumpQueue();
}

void PosterLoader::clear(brls::Image* view) {
    if (!view) return;
    // Remove pending jobs for this image.
    std::deque<PosterJob> removed;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::deque<PosterJob> kept;
        while (!g_queue.empty()) {
            auto job = std::move(g_queue.front());
            g_queue.pop_front();
            if (job.image == view) removed.push_back(std::move(job));
            else kept.push_back(std::move(job));
        }
        g_queue.swap(kept);
    }
    for (auto& job : removed) unlockJob(job);
    view->clear();
}

#endif

void PosterLoader::shutdown() {
    std::deque<PosterJob> cancelled;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_batch;
        cancelled.swap(g_queue);
    }
    for (auto& job : cancelled) unlockJob(job);
}

} // namespace lunar::ui
