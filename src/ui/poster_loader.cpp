#include "poster_loader.h"

#include "../api/http_client.h"
#include "../common.h"
#include "../diagnostics.h"
#include "../platform/network_worker.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __SWITCH__
#include <dirent.h>
#include <sys/stat.h>
#endif

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

constexpr size_t kPosterMemoryCacheLimit = 8 * 1024 * 1024;
constexpr size_t kMaximumPosterBytes = 1024 * 1024;
constexpr size_t kPosterDiskCacheLimit = 128 * 1024 * 1024;
constexpr size_t kPosterDiskCacheTrimTarget = 96 * 1024 * 1024;

struct CachedPoster {
    std::shared_ptr<const std::string> bytes;
    uint64_t last_used = 0;
};

std::unordered_map<std::string, CachedPoster> g_memory_cache;
size_t g_memory_cache_bytes = 0;
uint64_t g_cache_access = 0;
bool g_disk_cache_pruned = false;
size_t g_disk_cache_writes = 0;

std::string hashPosterUrl(const std::string& url) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : url) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

std::string cachedPosterPath(const std::string& url) {
    return std::string(lunar::get_cover_cache_dir()) + "/" +
           hashPosterUrl(url) + ".img";
}

bool hasImageSignature(const std::string& bytes) {
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xff &&
        static_cast<unsigned char>(bytes[1]) == 0xd8 &&
        static_cast<unsigned char>(bytes[2]) == 0xff) return true;
    static constexpr unsigned char png[] =
        {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    if (bytes.size() >= sizeof(png) &&
        std::equal(std::begin(png), std::end(png),
                   reinterpret_cast<const unsigned char*>(bytes.data()))) return true;
    return bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 &&
           bytes.compare(8, 4, "WEBP") == 0;
}

void ensurePosterCacheDirectory() {
#ifdef __SWITCH__
    ::mkdir("sdmc:/switch", 0777);
    ::mkdir("sdmc:/switch/LunarNX", 0777);
    ::mkdir("sdmc:/switch/LunarNX/cache", 0777);
    ::mkdir(lunar::get_cover_cache_dir(), 0777);
#endif
}

std::shared_ptr<const std::string> readDiskPoster(const std::string& url) {
    std::ifstream input(cachedPosterPath(url), std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto size = input.tellg();
    if (size <= 0 || static_cast<size_t>(size) > kMaximumPosterBytes) return {};
    input.seekg(0);
    std::string bytes(static_cast<size_t>(size), '\0');
    input.read(bytes.data(), size);
    if (!input.good() || !hasImageSignature(bytes)) {
        input.close();
        std::remove(cachedPosterPath(url).c_str());
        return {};
    }
    return std::make_shared<const std::string>(std::move(bytes));
}

void pruneDiskPosterCache() {
#ifdef __SWITCH__
    if (g_disk_cache_pruned && g_disk_cache_writes % 32 != 0) return;
    g_disk_cache_pruned = true;
    DIR* directory = opendir(lunar::get_cover_cache_dir());
    if (!directory) return;
    struct Entry { std::string path; size_t size; time_t modified; };
    std::vector<Entry> entries;
    size_t total = 0;
    while (auto* item = readdir(directory)) {
        if (item->d_name[0] == '.') continue;
        const std::string path = std::string(lunar::get_cover_cache_dir()) + "/" + item->d_name;
        struct stat info {};
        if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
        const size_t size = static_cast<size_t>(info.st_size);
        entries.push_back({path, size, info.st_mtime});
        total += size;
    }
    closedir(directory);
    if (total <= kPosterDiskCacheLimit) return;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.modified < b.modified;
    });
    for (const auto& entry : entries) {
        if (total <= kPosterDiskCacheTrimTarget) break;
        if (std::remove(entry.path.c_str()) == 0) total -= entry.size;
    }
#endif
}

void writeDiskPoster(const std::string& url, const std::string& bytes) {
    if (!hasImageSignature(bytes)) return;
    ensurePosterCacheDirectory();
    ++g_disk_cache_writes;
    pruneDiskPosterCache();
    const std::string path = cachedPosterPath(url);
    const std::string temporary = path + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output.good()) {
        std::remove(temporary.c_str());
        return;
    }
    std::remove(path.c_str());
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
    }
}

std::shared_ptr<const std::string> findCachedPosterLocked(const std::string& url) {
    auto it = g_memory_cache.find(url);
    if (it == g_memory_cache.end()) return {};
    it->second.last_used = ++g_cache_access;
    return it->second.bytes;
}

void cachePosterLocked(const std::string& url,
                       const std::shared_ptr<const std::string>& bytes) {
    if (!bytes || bytes->empty() || bytes->size() > kMaximumPosterBytes) return;
    auto existing = g_memory_cache.find(url);
    if (existing != g_memory_cache.end()) {
        g_memory_cache_bytes -= existing->second.bytes->size();
    }
    g_memory_cache[url] = CachedPoster{bytes, ++g_cache_access};
    g_memory_cache_bytes += bytes->size();

    while (g_memory_cache_bytes > kPosterMemoryCacheLimit &&
           g_memory_cache.size() > 1) {
        auto oldest = std::min_element(
            g_memory_cache.begin(), g_memory_cache.end(),
            [](const auto& left, const auto& right) {
                return left.second.last_used < right.second.last_used;
            });
        if (oldest == g_memory_cache.end()) break;
        g_memory_cache_bytes -= oldest->second.bytes->size();
        g_memory_cache.erase(oldest);
    }
}

std::string posterThumbnailUrl(const std::string& url) {
    if (url.find("store-images.s-microsoft.com") == std::string::npos) {
        return url;
    }
    const char separator = url.find('?') == std::string::npos ? '?' : '&';
    return url + separator + "w=240&h=360&q=80&format=jpg";
}

bool isBatchCurrent(PosterLoader::BatchId batch) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return batch != 0 && batch == g_batch;
}

void replacePosterTexture(brls::Image* image,
                          const std::shared_ptr<const std::string>& bytes) {
    if (!image || !bytes || bytes->empty()) return;

    // Resource placeholders belong to Borealis' shared TextureCache. Keep the
    // old texture non-owning while replacing it, then transfer ownership only
    // if NanoVG actually created a distinct texture for this card.
    const int previous_texture = image->getTexture();
    image->setFreeTexture(false);
    image->setImageFromMem(
        reinterpret_cast<const unsigned char*>(bytes->data()),
        static_cast<int>(bytes->size()));
    if (image->getTexture() != previous_texture) {
        image->setFreeTexture(true);
    }
}

void unlockJob(PosterJob& job) {
    if (!job.image) return;
    job.image->ptrUnlock();
    job.image = nullptr;
}

struct UiCompletion {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
};

void runOnUiAndWait(std::function<void()> action) {
    auto completion = std::make_shared<UiCompletion>();
    brls::sync([action = std::move(action), completion]() {
        action();
        {
            std::lock_guard<std::mutex> lock(completion->mutex);
            completion->done = true;
        }
        completion->condition.notify_one();
    });
    std::unique_lock<std::mutex> lock(completion->mutex);
    completion->condition.wait(lock, [&completion]() { return completion->done; });
}

void runQueueWorker() {
    while (true) {
        PosterJob job;
        std::shared_ptr<const std::string> body;
        std::string url;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_queue.empty()) {
                g_running = false;
                lunar::diagnosticLog("poster", "queue worker idle");
                return;
            }
            job = std::move(g_queue.front());
            g_queue.pop_front();
            url = posterThumbnailUrl(job.url);
            body = findCachedPosterLocked(url);
        }

        if (!job.image || job.url.empty()) {
            unlockJob(job);
            continue;
        }

        auto* image = job.image;
        const uint32_t seq = job.seq;
        const auto batch = job.batch;
        lunar::diagnosticLog("poster", "process seq=%u batch=%u cached=%d url=%s",
                             seq, batch, body ? 1 : 0, url.c_str());

        if (!body) {
            lunar::diagnosticLog("poster", "download begin seq=%u", seq);
            body = readDiskPoster(url);
            if (!body) {
                lunar::api::HttpClient http;
                auto resp = http.get(url, {{"Accept", "image/*"}});
                if (resp.status_code != 200 || resp.body.empty()) {
                    lunar::diagnosticLog("poster", "download fail seq=%u status=%d",
                                         seq, resp.status_code);
                    runOnUiAndWait([image]() { image->ptrUnlock(); });
                    continue;
                }
                if (resp.body.size() > kMaximumPosterBytes ||
                    !hasImageSignature(resp.body)) {
                    lunar::diagnosticLog("poster", "download invalid seq=%u bytes=%zu",
                                         seq, resp.body.size());
                    runOnUiAndWait([image]() { image->ptrUnlock(); });
                    continue;
                }
                writeDiskPoster(url, resp.body);
                body = std::make_shared<const std::string>(std::move(resp.body));
            }
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                cachePosterLocked(url, body);
            }
            lunar::diagnosticLog("poster", "download ok seq=%u bytes=%zu", seq, body->size());
        }

        runOnUiAndWait([image, body, seq, batch]() {
            if (isBatchCurrent(batch)) {
                replacePosterTexture(image, body);
                lunar::diagnosticLog("poster", "apply texture seq=%u tex=%d",
                                     seq, image->getTexture());
            } else {
                lunar::diagnosticLog("poster", "skip stale seq=%u batch=%u",
                                     seq, batch);
            }
            image->ptrUnlock();
        });
    }
}

void startQueueWorker() {
    const bool started = lunar::platform::startNetworkWorker(
        "poster-load",
        []() { runQueueWorker(); },
        1 * 1024 * 1024);
    if (!started) {
        std::deque<PosterJob> failed;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_running = false;
            failed.swap(g_queue);
        }
        for (auto& job : failed) {
            if (job.image) {
                job.image->ptrUnlock();
                job.image = nullptr;
            }
        }
        lunar::diagnosticLog("poster", "queue worker start failed jobs=%zu",
                             failed.size());
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
    bool start_worker = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (batch == 0 || batch != g_batch) {
            lunar::diagnosticLog("poster", "ignore stale enqueue seq=%u batch=%u current=%u",
                                 seq, batch, g_batch);
            return;
        }

        view->ptrLock();
        g_queue.push_back(PosterJob{view, url, seq, batch});
        if (!g_running) {
            g_running = true;
            start_worker = true;
        }
        lunar::diagnosticLog("poster", "enqueue seq=%u batch=%u q=%zu url=%s",
                             seq, batch, g_queue.size(), url.c_str());
    }
    if (start_worker) startQueueWorker();
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
        g_memory_cache.clear();
        g_memory_cache_bytes = 0;
    }
    for (auto& job : cancelled) unlockJob(job);
}

} // namespace lunar::ui
