#include "http_client.h"
#include "../diagnostics.h"
#include <curl/curl.h>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace lunar::api {

#ifndef LUNARNX_CURL_FORCE_MBEDTLS
#define LUNARNX_CURL_FORCE_MBEDTLS 1
#endif

#ifndef LUNARNX_CURL_VERIFY_SSL
#define LUNARNX_CURL_VERIFY_SSL 1
#endif

#ifndef LUNARNX_CURL_PROVIDER
#define LUNARNX_CURL_PROVIDER "unknown"
#endif

#ifndef LUNARNX_CURL_VERBOSE
#define LUNARNX_CURL_VERBOSE 0
#endif

#ifndef LUNARNX_CURL_TIMEOUT_MS
#define LUNARNX_CURL_TIMEOUT_MS 30000
#endif

namespace {
std::once_flag curl_init_once;

#if defined(__SWITCH__) && LUNARNX_CURL_VERBOSE
static int curl_debug_callback(CURL*, curl_infotype type, char* data, size_t size, void*) {
    if (type != CURLINFO_TEXT || data == nullptr || size == 0) {
        return 0;
    }

    std::string line(data, size);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    if (!line.empty()) {
        lunar::diagnosticLog("http-curl", "%s", line.c_str());
    }
    return 0;
}
#endif

static const char* curl_error_text(CURLcode res, char* error_buffer) {
    if (error_buffer && error_buffer[0] != '\0') {
        return error_buffer;
    }
    return curl_easy_strerror(res);
}

static curl_slist* append_header(curl_slist* hlist, const std::string& key,
                                 const std::string& value) {
    std::string line = value.empty() ? key + ":" : key + ": " + value;
    curl_slist* next = curl_slist_append(hlist, line.c_str());
    return next ? next : hlist;
}

static curl_slist* build_header_list(std::map<std::string, std::string> headers) {
    if (headers.find("Accept") == headers.end()) {
        headers["Accept"] = "*/*";
    }
    if (headers.find("Expect") == headers.end()) {
        headers["Expect"] = "";
    }

    curl_slist* hlist = nullptr;
    for (const auto& [key, value] : headers) {
        hlist = append_header(hlist, key, value);
    }
    return hlist;
}

static void log_curl_result(CURL* curl, const char* method, const std::string& url,
                            CURLcode res, const char* error_text) {
    long status = 0;
    long os_errno = 0;
    long ssl_verify = 0;
    long primary_port = 0;
    char* primary_ip = nullptr;
    double namelookup = 0.0;
    double connect = 0.0;
    double appconnect = 0.0;
    double starttransfer = 0.0;
    double total = 0.0;
    double downloaded = 0.0;

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &os_errno);
    curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &ssl_verify);
    curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &primary_ip);
    curl_easy_getinfo(curl, CURLINFO_PRIMARY_PORT, &primary_port);
    curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &namelookup);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect);
    curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &appconnect);
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &starttransfer);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &downloaded);

    lunar::diagnosticLog(
        "http",
        "%s %s result=%d status=%ld errno=%ld ssl_verify=%ld ip=%s port=%ld "
        "time_dns=%.3f time_connect=%.3f time_tls=%.3f time_first_byte=%.3f time_total=%.3f downloaded=%.0f error=%s",
        method,
        url.c_str(),
        static_cast<int>(res),
        status,
        os_errno,
        ssl_verify,
        primary_ip ? primary_ip : "",
        primary_port,
        namelookup,
        connect,
        appconnect,
        starttransfer,
        total,
        downloaded,
        res == CURLE_OK ? "" : error_text);
}
}

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* body = static_cast<std::string*>(userp);
    size_t total = size * nmemb;
    body->append(static_cast<const char*>(contents), total);
    return total;
}

static size_t header_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userp);
    size_t total = size * nmemb;
    std::string line(static_cast<const char*>(contents), total);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ')
            value.erase(0, 1);
        (*headers)[key] = value;
    }
    return total;
}

static int progress_callback(void* userp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* cancel = static_cast<HttpClient::CancelCallback*>(userp);
    return (cancel && *cancel && (*cancel)()) ? 1 : 0;
}

static void apply_common_options(CURL* curl, const std::string& url, struct curl_slist* hlist,
                                 std::string* body, std::map<std::string, std::string>* headers,
                                 HttpClient::CancelCallback* cancel, char* error_buffer) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, headers);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    constexpr long timeout_seconds = (LUNARNX_CURL_TIMEOUT_MS + 999L) / 1000L;
    constexpr long connect_timeout_seconds = timeout_seconds < 10L ? timeout_seconds : 10L;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(LUNARNX_CURL_TIMEOUT_MS));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_seconds * 1000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout_seconds);
    // Abort if the transfer stalls (common on /v2/titles at ~1MB under bad routes).
    // This lets callers fall back to recent titles instead of waiting the full timeout.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);   // <1 KiB/s
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);      // for 20 seconds
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "LunarNX");
#if defined(__SWITCH__)
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 16384L);
#endif
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, LUNARNX_CURL_VERIFY_SSL ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, LUNARNX_CURL_VERIFY_SSL ? 2L : 0L);
#if defined(__SWITCH__) && LUNARNX_CURL_VERBOSE
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_callback);
#endif
    if (cancel && *cancel) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
    }
}

HttpClient::HttpClient() {
    std::call_once(curl_init_once, []() {
#if LUNARNX_CURL_FORCE_MBEDTLS && LIBCURL_VERSION_NUM >= 0x075600
        curl_global_sslset(CURLSSLBACKEND_MBEDTLS, nullptr, nullptr);
#endif
        curl_global_init(CURL_GLOBAL_DEFAULT);
        lunar::diagnosticLog("http",
                             "curl init: %s provider=%s force_mbedtls=%d verify_ssl=%d verbose=%d timeout_ms=%d",
                             curl_version(), LUNARNX_CURL_PROVIDER,
                             LUNARNX_CURL_FORCE_MBEDTLS, LUNARNX_CURL_VERIFY_SSL,
                             LUNARNX_CURL_VERBOSE, LUNARNX_CURL_TIMEOUT_MS);
    });
    curl_handle_ = curl_easy_init();
}

HttpClient::~HttpClient() {
    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
        curl_handle_ = nullptr;
    }
}

HttpResponse HttpClient::get(const std::string& url,
                              const std::map<std::string, std::string>& headers,
                              CancelCallback cancel) {
    std::lock_guard<std::mutex> lock(mutex_);
    HttpResponse response;
    body_buf_.clear();
    resp_headers_.clear();
    char error_buffer[CURL_ERROR_SIZE] = {};

    auto merged = default_headers_;
    for (const auto& [k, v] : headers) merged[k] = v;
    struct curl_slist* hlist = build_header_list(std::move(merged));

    CURL* curl = static_cast<CURL*>(curl_handle_);
    if (!curl) {
        response.network_error = true;
        response.error_message = "curl init failed";
        lunar::diagnosticLog("http", "GET %s failed: %s",
                             url.c_str(), response.error_message.c_str());
        curl_slist_free_all(hlist);
        return response;
    }
    curl_easy_reset(curl);
    lunar::diagnosticLog("http", "GET %s begin", url.c_str());
    apply_common_options(curl, url, hlist, &body_buf_, &resp_headers_, &cancel, error_buffer);

    CURLcode res = curl_easy_perform(curl);
    const char* error_text = curl_error_text(res, error_buffer);
    log_curl_result(curl, "GET", url, res, error_text);
    curl_slist_free_all(hlist);
    // Always capture HTTP status when the peer started responding, even on timeout.
    {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status > 0) response.status_code = static_cast<int>(status);
    }
    if (res != CURLE_OK) {
        response.network_error = true;
        response.error_message = error_text;
        fprintf(stderr, "[http] GET %s failed: %s\n", url.c_str(), error_text);
        lunar::diagnosticLog("http", "GET %s failed: %s body_len=%zu status=%d",
                             url.c_str(), response.error_message.c_str(),
                             body_buf_.size(), response.status_code);
    }
    response.body = std::move(body_buf_);
    response.headers = std::move(resp_headers_);
    return response;
}

HttpResponse HttpClient::post(const std::string& url,
                               const std::string& body,
                               const std::map<std::string, std::string>& headers,
                               CancelCallback cancel) {
    std::lock_guard<std::mutex> lock(mutex_);
    HttpResponse response;
    body_buf_.clear();
    resp_headers_.clear();
    char error_buffer[CURL_ERROR_SIZE] = {};

    auto merged = default_headers_;
    for (const auto& [k, v] : headers) merged[k] = v;
    struct curl_slist* hlist = build_header_list(std::move(merged));

    CURL* curl = static_cast<CURL*>(curl_handle_);
    if (!curl) {
        response.network_error = true;
        response.error_message = "curl init failed";
        lunar::diagnosticLog("http", "POST %s failed: %s",
                             url.c_str(), response.error_message.c_str());
        curl_slist_free_all(hlist);
        return response;
    }
    curl_easy_reset(curl);
    lunar::diagnosticLog("http", "POST %s begin body_len=%zu", url.c_str(), body.size());
    apply_common_options(curl, url, hlist, &body_buf_, &resp_headers_, &cancel, error_buffer);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

    CURLcode res = curl_easy_perform(curl);
    const char* error_text = curl_error_text(res, error_buffer);
    log_curl_result(curl, "POST", url, res, error_text);
    curl_slist_free_all(hlist);
    // Always capture HTTP status when the peer started responding, even on timeout.
    {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status > 0) response.status_code = static_cast<int>(status);
    }
    if (res != CURLE_OK) {
        response.network_error = true;
        response.error_message = error_text;
        fprintf(stderr, "[http] POST %s failed: %s\n", url.c_str(), error_text);
        lunar::diagnosticLog("http", "POST %s failed: %s body_len=%zu status=%d",
                             url.c_str(), response.error_message.c_str(),
                             body_buf_.size(), response.status_code);
    }
    response.body = std::move(body_buf_);
    response.headers = std::move(resp_headers_);
    return response;
}

HttpResponse HttpClient::del(const std::string& url,
                              const std::map<std::string, std::string>& headers,
                              CancelCallback cancel) {
    std::lock_guard<std::mutex> lock(mutex_);
    HttpResponse response;
    body_buf_.clear();
    resp_headers_.clear();
    char error_buffer[CURL_ERROR_SIZE] = {};

    auto merged = default_headers_;
    for (const auto& [k, v] : headers) merged[k] = v;
    struct curl_slist* hlist = build_header_list(std::move(merged));

    CURL* curl = static_cast<CURL*>(curl_handle_);
    if (!curl) {
        response.network_error = true;
        response.error_message = "curl init failed";
        lunar::diagnosticLog("http", "DELETE %s failed: %s",
                             url.c_str(), response.error_message.c_str());
        curl_slist_free_all(hlist);
        return response;
    }
    curl_easy_reset(curl);
    lunar::diagnosticLog("http", "DELETE %s begin", url.c_str());
    apply_common_options(curl, url, hlist, &body_buf_, &resp_headers_, &cancel, error_buffer);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

    CURLcode res = curl_easy_perform(curl);
    const char* error_text = curl_error_text(res, error_buffer);
    log_curl_result(curl, "DELETE", url, res, error_text);
    curl_slist_free_all(hlist);
    // Always capture HTTP status when the peer started responding, even on timeout.
    {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status > 0) response.status_code = static_cast<int>(status);
    }
    if (res != CURLE_OK) {
        response.network_error = true;
        response.error_message = error_text;
        fprintf(stderr, "[http] DELETE %s failed: %s\n", url.c_str(), error_text);
        lunar::diagnosticLog("http", "DELETE %s failed: %s body_len=%zu status=%d",
                             url.c_str(), response.error_message.c_str(),
                             body_buf_.size(), response.status_code);
    }
    response.body = std::move(body_buf_);
    response.headers = std::move(resp_headers_);
    return response;
}

void HttpClient::setDefaultHeader(const std::string& key, const std::string& value) {
    default_headers_[key] = value;
}

} // namespace lunar::api
