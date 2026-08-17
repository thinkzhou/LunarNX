#include "dev_bridge_client.h"

#include "../api/http_client.h"
#include "../diagnostics.h"

#include <cJSON.h>
#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#include <cstdio>
#include <cerrno>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <array>

#ifdef __SWITCH__
#include <switch.h>
#endif

#ifndef LUNARNX_DEV_UPLOAD_TOKEN
#define LUNARNX_DEV_UPLOAD_TOKEN ""
#endif

#ifndef LUNARNX_GIT_COMMIT
#define LUNARNX_GIT_COMMIT "unknown"
#endif

#ifndef LUNARNX_CURL_VERIFY_SSL
#define LUNARNX_CURL_VERIFY_SSL 1
#endif

namespace lunar::app {
namespace {

constexpr long long kMaxNroBytes = 25LL * 1024LL * 1024LL;
constexpr long long kMaxLogBytes = 2LL * 1024LL * 1024LL;
constexpr long kDownloadTimeoutSeconds = 30L * 60L;
constexpr long kDownloadLowSpeedBytesPerSecond = 512L;
constexpr long kDownloadLowSpeedSeconds = 3L * 60L;

std::string responseHeader(const api::HttpResponse& response,
                           const char* requested_name) {
    for (const auto& [name, value] : response.headers) {
        if (name.size() != std::strlen(requested_name)) continue;
        bool matches = true;
        for (size_t i = 0; i < name.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(name[i])) !=
                std::tolower(static_cast<unsigned char>(requested_name[i]))) {
                matches = false;
                break;
            }
        }
        if (matches) return value;
    }
    return "missing";
}

std::string responsePreview(const std::string& body) {
    constexpr size_t kPreviewBytes = 96;
    std::string preview;
    preview.reserve(std::min(body.size(), kPreviewBytes));
    for (size_t i = 0; i < body.size() && i < kPreviewBytes; ++i) {
        const unsigned char c = static_cast<unsigned char>(body[i]);
        if (c == '\r' || c == '\n' || c == '\t') preview.push_back(' ');
        else if (c >= 0x20 && c <= 0x7e) preview.push_back(static_cast<char>(c));
        else preview.push_back('?');
    }
    return preview;
}

int sha256Starts(mbedtls_sha256_context* context) {
#if defined(MBEDTLS_VERSION_MAJOR) && MBEDTLS_VERSION_MAJOR >= 3
    return mbedtls_sha256_starts(context, 0);
#else
    return mbedtls_sha256_starts_ret(context, 0);
#endif
}

int sha256Update(mbedtls_sha256_context* context,
                 const unsigned char* data, size_t size) {
#if defined(MBEDTLS_VERSION_MAJOR) && MBEDTLS_VERSION_MAJOR >= 3
    return mbedtls_sha256_update(context, data, size);
#else
    return mbedtls_sha256_update_ret(context, data, size);
#endif
}

int sha256Finish(mbedtls_sha256_context* context, unsigned char digest[32]) {
#if defined(MBEDTLS_VERSION_MAJOR) && MBEDTLS_VERSION_MAJOR >= 3
    return mbedtls_sha256_finish(context, digest);
#else
    return mbedtls_sha256_finish_ret(context, digest);
#endif
}

std::string jsonString(cJSON* object, const char* key) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

bool parseBuild(cJSON* item, DevBuild& build) {
    if (!cJSON_IsObject(item)) return false;
    build.version = jsonString(item, "version");
    build.notes = jsonString(item, "notes");
    build.build_id = jsonString(item, "build_id");
    build.git_commit = jsonString(item, "git_commit");
    build.published_at = jsonString(item, "published_at");
    build.sha256 = jsonString(item, "sha256");
    build.download_url = jsonString(item, "download_url");
    build.compressed_download_url = jsonString(item, "compressed_download_url");
    cJSON* size = cJSON_GetObjectItemCaseSensitive(item, "size");
    cJSON* compressed_size = cJSON_GetObjectItemCaseSensitive(item, "compressed_size");
    build.size = cJSON_IsNumber(size) ? static_cast<long long>(size->valuedouble) : 0;
    build.compressed_size = cJSON_IsNumber(compressed_size)
        ? static_cast<long long>(compressed_size->valuedouble) : 0;
    const bool lowercase_sha = build.sha256.size() == 64 &&
        build.sha256.find_first_not_of("0123456789abcdef") == std::string::npos;
    const std::string expected_path = "/dev/builds/" + build.sha256 + ".nro";
    const bool valid_download_path = build.download_url.size() >= expected_path.size() &&
        build.download_url.compare(build.download_url.size() - expected_path.size(),
                                   expected_path.size(), expected_path) == 0;
    if (!build.version.empty() && lowercase_sha && valid_download_path &&
        build.size > 0 && build.size <= kMaxNroBytes) {
        // Historical manifests contain the old workers.dev origin. Always route
        // downloads through the configured custom domain after validating the
        // content-addressed path.
        build.download_url = std::string(DevBridgeClient::kBaseUrl) + expected_path;
        const std::string expected_compressed_path = expected_path + ".gz";
        const bool valid_compressed_path = build.compressed_download_url.size() >=
                expected_compressed_path.size() &&
            build.compressed_download_url.compare(
                build.compressed_download_url.size() - expected_compressed_path.size(),
                expected_compressed_path.size(), expected_compressed_path) == 0 &&
            build.compressed_size > 0 && build.compressed_size <= kMaxNroBytes;
        if (valid_compressed_path) {
            build.compressed_download_url =
                std::string(DevBridgeClient::kBaseUrl) + expected_compressed_path;
        } else {
            build.compressed_download_url.clear();
            build.compressed_size = 0;
        }
        return true;
    }
    return false;
}

struct DownloadState {
    FILE* file = nullptr;
    long long bytes = 0;
    mbedtls_sha256_context sha{};
    bool write_failed = false;
    DevBridgeClient::ProgressCallback progress;
    int last_percent = -1;
};

size_t downloadWrite(void* data, size_t size, size_t count, void* user) {
    auto* state = static_cast<DownloadState*>(user);
    const size_t bytes = size * count;
    if (!state || !state->file || state->bytes + static_cast<long long>(bytes) > kMaxNroBytes) {
        if (state) state->write_failed = true;
        return 0;
    }
    const size_t written = std::fwrite(data, 1, bytes, state->file);
    if (written != bytes) state->write_failed = true;
    if (written > 0) {
        sha256Update(&state->sha,
            static_cast<const unsigned char*>(data), written);
        state->bytes += static_cast<long long>(written);
    }
    return written;
}

size_t responseWrite(char* data, size_t size, size_t count, void* user) {
    const size_t bytes = size * count;
    auto* body = static_cast<std::string*>(user);
    if (!body) return 0;
    try {
        body->append(data, bytes);
    } catch (...) {
        return 0;
    }
    return bytes;
}

int downloadProgress(void* user, curl_off_t total, curl_off_t downloaded,
                     curl_off_t, curl_off_t) {
    auto* state = static_cast<DownloadState*>(user);
    if (!state || !state->progress) return 0;
    const long long expected = total > 0 ? static_cast<long long>(total) : 0;
    const int percent = expected > 0
        ? static_cast<int>((static_cast<long long>(downloaded) * 100) / expected)
        : -1;
    if (percent != state->last_percent) {
        state->last_percent = percent;
        state->progress(static_cast<long long>(downloaded), expected);
    }
    return 0;
}

std::string hexDigest(const unsigned char digest[32]) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return result;
}

bool replaceFile(const std::string& target, const std::string& temporary,
                 std::string& error) {
    const std::string backup = target + ".backup";
    const std::string previous_backup = backup + ".previous";
    if (std::remove(previous_backup.c_str()) != 0 && errno != ENOENT) {
        error = "Could not clear the previous backup archive";
        return false;
    }
    const bool had_backup = std::rename(backup.c_str(), previous_backup.c_str()) == 0;
    if (std::rename(target.c_str(), backup.c_str()) != 0) {
        if (had_backup) std::rename(previous_backup.c_str(), backup.c_str());
        error = "Could not back up the current NRO";
        return false;
    }
    if (std::rename(temporary.c_str(), target.c_str()) != 0) {
        const bool restored_current = std::rename(backup.c_str(), target.c_str()) == 0;
        if (had_backup) std::rename(previous_backup.c_str(), backup.c_str());
        if (!restored_current) {
            error = "Update failed and the current NRO could not be restored; use the backup file";
            return false;
        }
        error = "Could not replace the current NRO";
        return false;
    }
    if (had_backup) std::remove(previous_backup.c_str());
    return true;
}

std::string deviceIdPath() {
    return "sdmc:/switch/LunarNX/device_id.txt";
}

std::string loadOrCreateDeviceId() {
    const std::string path = deviceIdPath();
    std::array<char, 33> saved{};
    if (FILE* input = std::fopen(path.c_str(), "rb")) {
        const size_t read = std::fread(saved.data(), 1, 32, input);
        std::fclose(input);
        if (read == 32 && std::string(saved.data(), read).find_first_not_of(
                "0123456789abcdef") == std::string::npos) {
            return std::string(saved.data(), read);
        }
    }
    std::array<unsigned char, 16> random{};
#ifdef __SWITCH__
    randomGet(random.data(), random.size());
#else
    for (size_t i = 0; i < random.size(); ++i) random[i] = static_cast<unsigned char>(i);
#endif
    static constexpr char hex[] = "0123456789abcdef";
    std::string id(32, '0');
    for (size_t i = 0; i < random.size(); ++i) {
        id[i * 2] = hex[random[i] >> 4];
        id[i * 2 + 1] = hex[random[i] & 0x0f];
    }
    if (FILE* output = std::fopen(path.c_str(), "wb")) {
        std::fwrite(id.data(), 1, id.size(), output);
        std::fclose(output);
    }
    return id;
}

} // namespace

bool DevBridgeClient::fetchVersions(std::vector<DevBuild>& builds,
                                    std::string& error) const {
    api::HttpClient http;
    const auto response = http.get(std::string(kBaseUrl) + "/dev/versions.json");
    if (response.network_error || response.status_code != 200) {
        error = response.network_error ? response.error_message
                                       : "Server returned HTTP " + std::to_string(response.status_code);
        return false;
    }
    cJSON* root = cJSON_Parse(response.body.c_str());
    cJSON* versions = root ? cJSON_GetObjectItemCaseSensitive(root, "versions") : nullptr;
    if (!cJSON_IsArray(versions)) {
        long long parse_offset = -1;
        if (!root) {
            const char* parse_error = cJSON_GetErrorPtr();
            const char* body_start = response.body.c_str();
            const char* body_end = body_start + response.body.size();
            if (parse_error && parse_error >= body_start && parse_error <= body_end) {
                parse_offset = static_cast<long long>(parse_error - body_start);
            }
        }
        const std::string content_type = responseHeader(response, "content-type");
        const std::string preview = responsePreview(response.body);
        persistentEventLog(
            "dev-index",
            "invalid status=%d bytes=%zu content_type=%s parse_offset=%lld preview=%s",
            response.status_code, response.body.size(), content_type.c_str(),
            parse_offset, preview.c_str());
        cJSON_Delete(root);
        error = "Invalid version index (" + std::to_string(response.body.size()) +
            " bytes, " + content_type + ")";
        return false;
    }
    builds.clear();
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, versions) {
        DevBuild build;
        if (parseBuild(item, build)) builds.push_back(std::move(build));
    }
    cJSON_Delete(root);
    if (builds.empty()) {
        error = "No downloadable versions";
        return false;
    }
    return true;
}

bool DevBridgeClient::install(const DevBuild& build, const std::string& target_path,
                              std::string& error, ProgressCallback progress) const {
    if (target_path.rfind("sdmc:/", 0) != 0 ||
        target_path.size() < 5 || target_path.substr(target_path.size() - 4) != ".nro" ||
        build.size <= 0 || build.size > kMaxNroBytes ||
        build.sha256.size() != 64) {
        error = "Invalid build metadata or application path";
        return false;
    }
    FILE* current = std::fopen(target_path.c_str(), "rb");
    if (!current) {
        error = "The running NRO path does not exist";
        return false;
    }
    std::fclose(current);
    const std::string temporary = target_path + ".update";
    FILE* output = std::fopen(temporary.c_str(), "wb");
    if (!output) {
        error = "Could not create the update file";
        return false;
    }

    DownloadState state;
    state.file = output;
    state.progress = std::move(progress);
    mbedtls_sha256_init(&state.sha);
    sha256Starts(&state.sha);
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(output);
        std::remove(temporary.c_str());
        mbedtls_sha256_free(&state.sha);
        error = "Could not initialize download";
        return false;
    }
    const std::string& download_url = build.compressed_download_url.empty()
        ? build.download_url : build.compressed_download_url;
    curl_easy_setopt(curl, CURLOPT_URL, download_url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kDownloadTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, kDownloadLowSpeedBytesPerSecond);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kDownloadLowSpeedSeconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "LunarNX-Updater");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, LUNARNX_CURL_VERIFY_SSL ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, LUNARNX_CURL_VERIFY_SSL ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, downloadWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, downloadProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    const bool close_ok = std::fclose(output) == 0;

    unsigned char digest[32]{};
    sha256Finish(&state.sha, digest);
    mbedtls_sha256_free(&state.sha);
    if (result != CURLE_OK || status != 200 || state.write_failed || !close_ok ||
        state.bytes != build.size || hexDigest(digest) != build.sha256) {
        std::remove(temporary.c_str());
        error = result != CURLE_OK ? curl_easy_strerror(result)
            : status != 200 ? "Download returned HTTP " + std::to_string(status)
            : "Downloaded file failed size or SHA-256 verification";
        diagnosticLog("dev-update", "download rejected version=%s status=%ld bytes=%lld",
                      build.version.c_str(), status, state.bytes);
        return false;
    }
    if (!replaceFile(target_path, temporary, error)) return false;
    diagnosticLog("dev-update", "installed version=%s bytes=%lld target=%s",
                  build.version.c_str(), state.bytes, target_path.c_str());
    return true;
}

bool DevBridgeClient::uploadLog(const std::string& log_path, std::string& log_id,
                                std::string& error) const {
    if (std::strlen(LUNARNX_DEV_UPLOAD_TOKEN) == 0) {
        error = "This build does not contain a development upload token";
        return false;
    }
    FILE* input = std::fopen(log_path.c_str(), "rb");
    if (!input) {
        error = "No log file is available";
        return false;
    }
    std::fseek(input, 0, SEEK_END);
    const long size = std::ftell(input);
    std::rewind(input);
    if (size <= 0 || size > kMaxLogBytes) {
        std::fclose(input);
        error = "Log is empty or exceeds 2 MiB";
        return false;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(input);
        error = "Could not initialize upload";
        return false;
    }
    const std::string authorization = "Authorization: Bearer " +
        std::string(LUNARNX_DEV_UPLOAD_TOKEN);
    const std::string version = "X-LunarNX-Build: " + std::string(LUNARNX_VERSION);
    const std::string commit = "X-LunarNX-Commit: " + std::string(LUNARNX_GIT_COMMIT);
    const std::string device = "X-LunarNX-Device: " + loadOrCreateDeviceId();
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, authorization.c_str());
    headers = curl_slist_append(headers, version.c_str());
    headers = curl_slist_append(headers, commit.c_str());
    headers = curl_slist_append(headers, device.c_str());
    headers = curl_slist_append(headers, "Content-Type: text/plain; charset=utf-8");
    const std::string url = std::string(kBaseUrl) + "/dev/logs";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, input);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(size));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, LUNARNX_CURL_VERIFY_SSL ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, LUNARNX_CURL_VERIFY_SSL ? 2L : 0L);
    std::string response_body;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, responseWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    std::fclose(input);
    if (result != CURLE_OK || status != 201) {
        error = result != CURLE_OK ? curl_easy_strerror(result)
                                  : "Upload returned HTTP " + std::to_string(status);
        return false;
    }
    cJSON* root = cJSON_Parse(response_body.c_str());
    log_id = root ? jsonString(root, "log_id") : "";
    cJSON_Delete(root);
    if (log_id.empty()) log_id = "uploaded";
    diagnosticLog("dev-log", "uploaded bytes=%ld log_id=%s", size, log_id.c_str());
    return true;
}

} // namespace lunar::app
