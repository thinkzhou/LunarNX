#include <curl/curl.h>
#include <switch.h>

#include <arpa/inet.h>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>
#include <string>
#include <vector>

#ifndef LUNARNX_NETPROBE_RAW_HTTP
#define LUNARNX_NETPROBE_RAW_HTTP 0
#endif

#ifndef LUNARNX_NETPROBE_CERTINFO
#define LUNARNX_NETPROBE_CERTINFO 0
#endif

#ifndef LUNARNX_NETPROBE_RAW_PUBLIC
#define LUNARNX_NETPROBE_RAW_PUBLIC 1
#endif

#ifndef LUNARNX_NETPROBE_RUN_CURL
#define LUNARNX_NETPROBE_RUN_CURL 1
#endif

#ifndef LUNARNX_NETPROBE_LOCAL_HOST
#define LUNARNX_NETPROBE_LOCAL_HOST ""
#endif

#ifndef LUNARNX_NETPROBE_LOCAL_PORT
#define LUNARNX_NETPROBE_LOCAL_PORT "18080"
#endif

namespace {

constexpr const char* LOG_DIR = "sdmc:/switch/LunarNXNetProbe";
constexpr const char* LOG_PATH = "sdmc:/switch/LunarNXNetProbe/netprobe.log";

struct Probe {
    const char* name;
    const char* url;
};

struct ResponseBody {
    std::string text;
};

std::recursive_mutex share_locks[CURL_LOCK_DATA_LAST];

void ensureLogDir() {
    mkdir("sdmc:/switch", 0777);
    mkdir(LOG_DIR, 0777);
}

void logLine(const char* format, ...) {
    ensureLogDir();

    FILE* file = std::fopen(LOG_PATH, "a");
    if (!file) {
        return;
    }

    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fprintf(file, "\n");
    std::fclose(file);

    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
    consoleUpdate(nullptr);
}

size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* body = static_cast<ResponseBody*>(userp);
    const size_t total = size * nmemb;
    body->text.append(static_cast<const char*>(contents), total);
    return total;
}

void lockCallback(CURL*, curl_lock_data data, curl_lock_access, void*) {
    if (data >= 0 && data < CURL_LOCK_DATA_LAST) {
        share_locks[data].lock();
    }
}

void unlockCallback(CURL*, curl_lock_data data, void*) {
    if (data >= 0 && data < CURL_LOCK_DATA_LAST) {
        share_locks[data].unlock();
    }
}

curl_slist* appendHeader(curl_slist* headers, const char* value) {
    curl_slist* next = curl_slist_append(headers, value);
    return next ? next : headers;
}

void logCurlResult(CURL* curl, const char* mode, const Probe& probe, CURLcode result,
                   const char* error_buffer, const ResponseBody& body) {
    long status = 0;
    long os_errno = 0;
    long ssl_verify = 0;
    long primary_port = 0;
    char* primary_ip = nullptr;
    double time_dns = 0.0;
    double time_connect = 0.0;
    double time_tls = 0.0;
    double time_first_byte = 0.0;
    double time_total = 0.0;
    double downloaded = 0.0;

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &os_errno);
    curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &ssl_verify);
    curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &primary_ip);
    curl_easy_getinfo(curl, CURLINFO_PRIMARY_PORT, &primary_port);
    curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &time_dns);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &time_connect);
    curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &time_tls);
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &time_first_byte);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &time_total);
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &downloaded);

    const char* error_text = "";
    if (result != CURLE_OK) {
        error_text = (error_buffer && error_buffer[0]) ? error_buffer : curl_easy_strerror(result);
    }

    logLine(
        "[%s] %s result=%d status=%ld errno=%ld ssl_verify=%ld ip=%s port=%ld "
        "dns=%.3f tcp=%.3f tls=%.3f first_byte=%.3f total=%.3f downloaded=%.0f body=%zu error=%s",
        mode,
        probe.name,
        static_cast<int>(result),
        status,
        os_errno,
        ssl_verify,
        primary_ip ? primary_ip : "",
        primary_port,
        time_dns,
        time_connect,
        time_tls,
        time_first_byte,
        time_total,
        downloaded,
        body.text.size(),
        error_text);
}

void applyMinimalOptions(CURL* curl, const Probe& probe, ResponseBody* body, char* error_buffer) {
    curl_easy_setopt(curl, CURLOPT_URL, probe.url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 4096L);
}

curl_slist* applyWiliwiliOptions(CURL* curl, CURLSH* share, const Probe& probe,
                                 ResponseBody* body, char* error_buffer) {
    curl_slist* headers = nullptr;
    headers = appendHeader(headers, "User-Agent: wiliwili");
    headers = appendHeader(headers, "Referer: https://www.bilibili.com/client");
    headers = appendHeader(headers, "Origin: https://www.bilibili.com");
    headers = appendHeader(headers, "Accept: */*");
    headers = appendHeader(headers, "Expect:");

    curl_easy_setopt(curl, CURLOPT_URL, probe.url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "wiliwili");
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 0L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#if LUNARNX_NETPROBE_CERTINFO
    curl_easy_setopt(curl, CURLOPT_CERTINFO, 1L);
#endif
    curl_easy_setopt(curl, CURLOPT_SHARE, share);
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 4096L);
    return headers;
}

void runProbe(CURLSH* share, const char* mode, const Probe& probe) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        logLine("[%s] %s curl_easy_init failed", mode, probe.name);
        return;
    }

    ResponseBody body;
    char error_buffer[CURL_ERROR_SIZE] = {};
    curl_slist* headers = nullptr;

    logLine("[%s] %s begin url=%s", mode, probe.name, probe.url);
    if (std::strcmp(mode, "wiliwili") == 0) {
        headers = applyWiliwiliOptions(curl, share, probe, &body, error_buffer);
    } else {
        applyMinimalOptions(curl, probe, &body, error_buffer);
    }

    CURLcode result = curl_easy_perform(curl);
    logCurlResult(curl, mode, probe, result, error_buffer, body);

    if (headers) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);
}

void runAllProbes() {
    const Probe probes[] = {
        {"baidu-http", "http://www.baidu.com/"},
        {"baidu-https", "https://www.baidu.com/"},
        {"bilibili-nav", "https://api.bilibili.com/x/web-interface/nav"},
        {"github-api", "https://api.github.com/repos/xfangfang/wiliwili/releases/latest"},
        {"qq-https", "https://www.qq.com/"},
        {"microsoft-oidc", "https://login.microsoftonline.com/consumers/v2.0/.well-known/openid-configuration"},
    };

    logLine("[probe] curl=%s", curl_version());
    curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    logLine("[probe] features=0x%08lx ssl=%s libz=%s",
            static_cast<unsigned long>(info ? info->features : 0),
            info && info->ssl_version ? info->ssl_version : "",
            info && info->libz_version ? info->libz_version : "");

    CURLSH* share = curl_share_init();
    if (share) {
        curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(share, CURLSHOPT_LOCKFUNC, lockCallback);
        curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, unlockCallback);
    }

    for (const auto& probe : probes) {
        runProbe(share, "minimal", probe);
    }

    if (share) {
        curl_share_cleanup(share);
    }
}

#if LUNARNX_NETPROBE_RAW_HTTP
void runRawHttpProbe(const char* name, const char* host, const char* port,
                     const char* path, const char* http_version, size_t recv_buffer_size = 1024) {
    logLine("[raw-http] %s begin host=%s port=%s path=%s version=%s recv_buf=%zu",
            name, host, port, path, http_version, recv_buffer_size);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    sockaddr_in numeric_addr{};
    addrinfo numeric_info{};

    const int gai = getaddrinfo(host, port, &hints, &result);
    if (gai != 0 || !result) {
        numeric_addr.sin_family = AF_INET;
        numeric_addr.sin_port = htons(static_cast<uint16_t>(std::strtoul(port, nullptr, 10)));
        if (inet_pton(AF_INET, host, &numeric_addr.sin_addr) != 1) {
            logLine("[raw-http] %s getaddrinfo failed gai=%d", name, gai);
            return;
        }

        numeric_info.ai_family = AF_INET;
        numeric_info.ai_socktype = SOCK_STREAM;
        numeric_info.ai_protocol = IPPROTO_TCP;
        numeric_info.ai_addr = reinterpret_cast<sockaddr*>(&numeric_addr);
        numeric_info.ai_addrlen = sizeof(numeric_addr);
        result = &numeric_info;
        logLine("[raw-http] %s getaddrinfo failed gai=%d; using numeric IPv4 fallback", name, gai);
    }

    int fd = -1;
    int connect_errno = 0;
    char ip[INET_ADDRSTRLEN] = {};
    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        auto* addr = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
        inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));

        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            connect_errno = errno;
            continue;
        }

        timeval timeout{};
        timeout.tv_sec = 10;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            connect_errno = 0;
            break;
        }

        connect_errno = errno;
        close(fd);
        fd = -1;
    }
    if (result != &numeric_info) {
        freeaddrinfo(result);
    }

    if (fd < 0) {
        logLine("[raw-http] %s connect failed errno=%d ip=%s", name, connect_errno, ip);
        return;
    }

    std::string request = "GET ";
    request += path;
    request += " ";
    request += http_version;
    request += "\r\nHost: ";
    request += host;
    request += "\r\nUser-Agent: LunarNXNetProbe\r\nAccept: */*\r\nConnection: close\r\n\r\n";

    const ssize_t sent = send(fd, request.data(), request.size(), 0);
    std::vector<char> buffer(recv_buffer_size + 1, 0);
    const ssize_t received = sent >= 0 ? recv(fd, buffer.data(), recv_buffer_size, 0) : -1;
    const int recv_errno = received < 0 ? errno : 0;
    close(fd);

    std::string first_line;
    if (received > 0) {
        first_line.assign(buffer.data(), static_cast<size_t>(received));
        size_t end = first_line.find('\n');
        if (end != std::string::npos) {
            first_line.resize(end);
        }
        while (!first_line.empty() && (first_line.back() == '\r' || first_line.back() == '\n')) {
            first_line.pop_back();
        }
    }

    logLine("[raw-http] %s sent=%zd received=%zd errno=%d ip=%s port=%s version=%s recv_buf=%zu first_line=%s",
            name, sent, received, recv_errno, ip, port, http_version, recv_buffer_size, first_line.c_str());
}
#endif

} // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    consoleInit(nullptr);
    ensureLogDir();
    std::remove(LOG_PATH);

    logLine("[probe] LunarNXNetProbe start");
    logLine("[probe] applet_type=%d", static_cast<int>(appletGetAppletType()));

    CURLcode init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    logLine("[probe] curl_global_init=%d", static_cast<int>(init_result));
    if (init_result == CURLE_OK) {
#if LUNARNX_NETPROBE_RAW_HTTP
        if (std::strlen(LUNARNX_NETPROBE_LOCAL_HOST) > 0) {
            const size_t local_recv_sizes[] = {1024, 8192, 16384, 32767, 32768, 32769, 65536};
            for (size_t recv_size : local_recv_sizes) {
                runRawHttpProbe("local-http10", LUNARNX_NETPROBE_LOCAL_HOST, LUNARNX_NETPROBE_LOCAL_PORT, "/", "HTTP/1.0", recv_size);
            }
            runRawHttpProbe("local-http11", LUNARNX_NETPROBE_LOCAL_HOST, LUNARNX_NETPROBE_LOCAL_PORT, "/", "HTTP/1.1", 1024);
        }
#if LUNARNX_NETPROBE_RAW_PUBLIC
        runRawHttpProbe("baidu-http10", "www.baidu.com", "80", "/", "HTTP/1.0");
        runRawHttpProbe("baidu-http11", "www.baidu.com", "80", "/", "HTTP/1.1");
        runRawHttpProbe("neverssl-http10", "neverssl.com", "80", "/", "HTTP/1.0");
        runRawHttpProbe("neverssl-http11", "neverssl.com", "80", "/", "HTTP/1.1");
        runRawHttpProbe("example-http10", "example.com", "80", "/", "HTTP/1.0");
        runRawHttpProbe("example-http11", "example.com", "80", "/", "HTTP/1.1");
        runRawHttpProbe("httpbin-http11", "httpbin.org", "80", "/get", "HTTP/1.1");
        runRawHttpProbe("github-http11", "github.com", "80", "/", "HTTP/1.1");
#endif
#endif
#if LUNARNX_NETPROBE_RUN_CURL
        runAllProbes();
#endif
    }
    curl_global_cleanup();

    logLine("[probe] done");
    consoleUpdate(nullptr);
    svcSleepThread(3000000000LL);
    consoleExit(nullptr);
    return 0;
}
