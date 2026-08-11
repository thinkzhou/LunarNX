#include <curl/curl.h>
#include <atomic>
#include <mutex>

namespace {

std::mutex& chiakiCurlMutex() {
    static std::mutex mutex;
    return mutex;
}

std::atomic<bool>& ryubingCompat() {
    static std::atomic<bool> enabled{false};
    return enabled;
}

std::unique_lock<std::mutex> chiakiCurlLock() {
    std::unique_lock<std::mutex> lock(chiakiCurlMutex(), std::defer_lock);
    if (ryubingCompat().load(std::memory_order_relaxed)) lock.lock();
    return lock;
}

} // namespace

#ifdef __SWITCH__
extern "C" CURL* __real_curl_easy_init(void);
#endif

extern "C" void lunarnx_chiaki_set_ryubing_compat(bool enabled) {
    ryubingCompat().store(enabled, std::memory_order_relaxed);
}

extern "C" bool lunarnx_chiaki_ryubing_compat_enabled(void) {
    return ryubingCompat().load(std::memory_order_relaxed);
}

extern "C" CURL* lunarnx_chiaki_curl_easy_init(void) {
    auto lock = chiakiCurlLock();
#ifdef __SWITCH__
    CURL* curl = __real_curl_easy_init();
#else
    CURL* curl = curl_easy_init();
#endif
    if (!curl) return nullptr;

#ifdef __SWITCH__
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
#endif
#if !LUNARNX_CURL_VERIFY_SSL
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#endif
    return curl;
}

#ifdef __SWITCH__
/* Akira Chiaki calls libcurl directly.  Link wrapping routes those calls
 * through the same Switch defaults used by LunarNX's own HTTP client. */
extern "C" CURL* __wrap_curl_easy_init(void) {
    return lunarnx_chiaki_curl_easy_init();
}
#endif

extern "C" CURLcode lunarnx_chiaki_curl_easy_perform(CURL* curl) {
    auto lock = chiakiCurlLock();
    return curl_easy_perform(curl);
}

extern "C" void lunarnx_chiaki_curl_easy_cleanup(CURL* curl) {
    auto lock = chiakiCurlLock();
    curl_easy_cleanup(curl);
}

extern "C" CURLcode lunarnx_chiaki_curl_ws_send(
    CURL* curl,
    const void* buffer,
    size_t buflen,
    size_t* sent,
    curl_off_t fragsize,
    unsigned int flags) {
    auto lock = chiakiCurlLock();
    return curl_ws_send(curl, buffer, buflen, sent, fragsize, flags);
}

extern "C" CURLcode lunarnx_chiaki_curl_ws_recv(
    CURL* curl,
    void* buffer,
    size_t buflen,
    size_t* recv,
    const struct curl_ws_frame** meta) {
    auto lock = chiakiCurlLock();
    return curl_ws_recv(curl, buffer, buflen, recv, meta);
}
