#include "stream_controller.h"
#include "xbox_ice_preferences.h"
#include "../api/api_constants.h"

#include "../common.h"
#include "../diagnostics.h"
#include "stream_profile.h"
#include "../platform/network_worker.h"
#include <cJSON.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <cstdio>

namespace lunar::app {

StreamController::StreamController() = default;

StreamController::~StreamController() {
    // Best-effort teardown. Avoid throwing across destructor boundaries.
    try {
        cancel_requested_ = true;
        stream_generation_.fetch_add(1);
        if (stream_session_) {
            stream_session_->stop(true);
            stream_session_.reset();
        }
        if (transport_) {
            transport_->disconnect();
            transport_.reset();
        }
        if (media_) {
            media_->shutdown();
            media_.reset();
        }
        if (rumble_) {
            rumble_->stop();
            rumble_.reset();
        }
        channels_.reset();
        session_client_.reset();
        api_.reset();
    } catch (...) {
        // swallow
    }
}


api::HttpClient& StreamController::http() {
    if (!http_) {
        http_ = std::make_unique<api::HttpClient>();
    }
    return *http_;
}

auth::AuthManager& StreamController::auth() {
    if (!auth_) {
        auth_ = std::make_unique<auth::AuthManager>(http());
    }
    return *auth_;
}

void StreamController::ensureStreamingComponents() {
    if (!stream_backend_) {
        stream_backend_ = stream::StreamBackendProvider::createDefault();
    }
    if (!media_) {
        media_ = std::make_unique<stream::MediaPipeline>(*stream_backend_);
    }
    if (!gamepad_) {
        gamepad_ = std::make_unique<input::GamepadReader>(
            input::ButtonMappingProfile::Xbox);
    }
    if (!xinput_) {
        xinput_ = std::make_unique<input::XInputEncoder>();
    }
    if (!rumble_) {
        rumble_ = std::make_unique<input::RumbleController>();
        rumble_->setEnabled(rumble_enabled_.load());
        rumble_->setStrengthPercent(rumble_strength_percent_.load());
    }
}

void StreamController::setState(StreamState state, const std::string& info) {
    state_ = state;
    if (state == StreamState::Error || state == StreamState::Disconnected) {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        last_stream_error_ = info;
    } else if (state == StreamState::Connecting || state == StreamState::Streaming) {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        last_stream_error_.clear();
    }
    StateCallback callback;
    {
        std::lock_guard<std::mutex> lock(state_callback_mutex_);
        callback = state_cb_;
    }
    if (callback) {
        callback(state, info);
    }
}

void StreamController::setStateCallback(StateCallback cb) {
    std::lock_guard<std::mutex> lock(state_callback_mutex_);
    state_cb_ = std::move(cb);
}

void StreamController::setDefaultVideoBackend(stream::VideoBackend backend) {
    default_video_backend_.store(backend);
    lunar::diagnosticLog("stream-controller",
                         "Default video backend set to %s",
                         stream::videoBackendName(backend));
}

stream::VideoBackend StreamController::getDefaultVideoBackend() const {
    return default_video_backend_.load();
}

void StreamController::setRumbleEnabled(bool enabled) {
    rumble_enabled_.store(enabled);
    if (rumble_) rumble_->setEnabled(enabled);
    lunar::diagnosticLog("stream-controller", "rumble enabled=%s",
                         enabled ? "true" : "false");
}

void StreamController::setRumbleStrengthPercent(int percent) {
    percent = std::clamp(percent, 0, 100);
    rumble_strength_percent_.store(percent);
    if (rumble_) rumble_->setStrengthPercent(percent);
    lunar::diagnosticLog("stream-controller", "rumble strength=%d%%", percent);
}

void StreamController::setForceRegionIp(const std::string& ip) {
    auth().setForceRegionIp(ip);
    lunar::diagnosticLog("stream-controller", "force_region_ip=%s",
                         ip.empty() ? "(default)" : ip.c_str());
}

std::string StreamController::getForceRegionIp() const {
    // const_cast-like access via mutable auth? auth() is non-const.
    // Provide const path by using const_cast on this for auth().
    return const_cast<StreamController*>(this)->auth().getForceRegionIp();
}

void StreamController::setPreferredGameLanguage(const std::string& locale) {
    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
    const std::string normalized = locale.empty() ? "en-US" : locale;
    if (preferred_game_language_ == normalized) return;
    preferred_game_language_ = normalized;
    cloud_titles_.clear();
    recent_cloud_titles_.clear();
    new_cloud_titles_.clear();
    last_cloud_title_error_.clear();
    lunar::diagnosticLog("stream-controller", "preferred game language=%s",
                         preferred_game_language_.c_str());
}

std::string StreamController::getPreferredGameLanguage() const {
    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
    return preferred_game_language_;
}

bool StreamController::startAuth() {
    setState(StreamState::Authenticating);
    return auth().startDeviceCodeAuth();
}

bool StreamController::pollAuth() {
    return pollAuthStatus() == auth::DeviceCodePollResult::Authenticated;
}

auth::DeviceCodePollResult StreamController::pollAuthStatus() {
    auto result = auth().pollForTokenResult();
    if (result == auth::DeviceCodePollResult::Authenticated) {
        auth().saveTokens(lunar::get_token_path());
    }
    return result;
}

int StreamController::getAuthPollIntervalSeconds() const {
    return auth_ ? auth_->getPollIntervalSeconds() : 5;
}

int StreamController::getDeviceCodeExpiresInSeconds() const {
    return auth_ ? auth_->getDeviceCodeExpiresInSeconds() : 900;
}

bool StreamController::hasCredentials() const {
    return mock_mode_.load() || (auth_ && auth_->hasSavedCredentials());
}

bool StreamController::loadTokens(const std::string& path) {
    return auth().loadTokens(path);
}

bool StreamController::saveTokens(const std::string& path) {
    return auth().saveTokens(path);
}

void StreamController::signOut() {
    signing_out_ = true;
    requestStreamStop();

    std::lock_guard<std::mutex> auth_lock(auth_operation_mutex_);
    std::lock_guard<std::mutex> operation_lock(stream_operation_mutex_);
    cleanupStreamResources(false);
    if (auth_) {
        auth_->clearTokens();
    }
    api_.reset();
    session_client_.reset();
    stream_session_.reset();
    {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        mock_mode_ = false;
        consoles_.clear();
        last_console_error_.clear();
        last_stream_error_.clear();
        session_id_.clear();
        streaming_ = false;
        input_router_.setOwner(input::StreamInputOwner::Game);
        guide_button_requested_ = false;
    }
    std::remove(lunar::get_token_path());
    std::remove(lunar::get_xbox_console_cache_path());
    std::remove(lunar::get_xbox_ice_preferences_path());
    setState(StreamState::Idle, "Signed out");
    signing_out_ = false;
}

std::string StreamController::getGamertag() const {
    return auth_ ? auth_->getGamertag() : "";
}

std::string StreamController::getAuthError() const {
    return auth_ ? auth_->getLastError() : "";
}

std::string StreamController::getAuthCode() const {
    return auth_ ? auth_->getUserCode() : "";
}

std::string StreamController::getAuthUrl() const {
    return auth_ ? auth_->getVerificationUri() : "";
}

bool StreamController::fetchConsoles() {
    std::lock_guard<std::mutex> auth_lock(auth_operation_mutex_);
    auto signout_requested = [this]() {
        return signing_out_.load() ||
            lunar::platform::networkWorkersShuttingDown();
    };

    if (mock_mode_.load()) {
        std::shared_ptr<api::XboxApiClient> api;
        {
            std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
            api = api_;
        }
        if (!api) {
            std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
            last_console_error_ = "Mock mode is not initialized.";
            lunar::diagnosticLog("stream-controller", "Mock fetch consoles blocked: no api");
            return false;
        }
        auto consoles = api->getConsoles(signout_requested);
        const bool found = !consoles.empty();
        const std::string api_error = api->getLastError();
        if (signout_requested()) {
            std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
            consoles_.clear();
            last_console_error_ = "Signing out...";
            lunar::diagnosticLog("stream-controller", "Mock fetch consoles cancelled by sign-out");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
            consoles_ = std::move(consoles);
            last_console_error_ = found
                ? ""
                : (api_error.empty()
                    ? "No mock Xbox consoles found."
                    : api_error);
            if (!last_console_error_.empty()) {
                lunar::diagnosticLog("stream-controller", "Mock fetch consoles failed: %s",
                                     last_console_error_.c_str());
            } else {
                lunar::diagnosticLog("stream-controller", "Mock fetch consoles succeeded count=%zu",
                                     consoles_.size());
            }
            return found;
        }
    }

    auto& auth_ref = auth();
    // If cloud still missing (e.g. previous InvalidCountry), force re-derive after region change.
    if (!auth_ref.hasCloudAccess() && !auth_ref.getForceRegionIp().empty()) {
        auth_ref.refreshStreamingTokens(true, signout_requested);
    }
    if (signout_requested() ||
        !auth_ref.refreshTokensIfNeeded(signout_requested) ||
        !auth_ref.isAuthenticated() ||
        signout_requested()) {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        consoles_.clear();
        api_.reset();
        if (signout_requested()) {
            last_console_error_ = "Signing out...";
        } else if (auth_ref.hasSavedCredentials()) {
            last_console_error_ = auth_ref.getLastError().empty()
                ? "Saved sign-in could not be refreshed. Check WiFi or sign in again."
                : auth_ref.getLastError();
        } else {
            last_console_error_ = "Please sign in again.";
        }
        lunar::diagnosticLog("stream-controller", "Fetch consoles blocked: %s",
                             last_console_error_.c_str());
        return false;
    }
    auth_ref.saveTokens(lunar::get_token_path());

    auto api = makeApiClient(SessionType::Home);
    if (!api) {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        consoles_.clear();
        last_console_error_ = "Could not create Xbox API client.";
        return false;
    }
    auto consoles = api->getConsoles(signout_requested);
    const bool found = !consoles.empty();
    const std::string api_error = api->getLastError();
    if (signout_requested()) {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        consoles_.clear();
        api_.reset();
        last_console_error_ = "Signing out...";
        lunar::diagnosticLog("stream-controller", "Fetch consoles cancelled by sign-out");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        consoles_ = std::move(consoles);
        if (!streaming_) {
            api_ = std::move(api);
        }
        last_console_error_ = found
            ? ""
            : (api_error.empty()
                ? "No Xbox consoles found for this account."
                : api_error);
        if (!last_console_error_.empty()) {
            lunar::diagnosticLog("stream-controller", "Fetch consoles failed: %s",
                                 last_console_error_.c_str());
        } else {
            lunar::diagnosticLog("stream-controller", "Fetch consoles succeeded count=%zu",
                                 consoles_.size());
        }
    }
    // Snapshot under stream_lifecycle_mutex_, then serialize and write after
    // releasing it. SD-card latency must not block other lifecycle readers.
    if (found) saveConsoleCache();
    return found;
}

std::vector<api::XboxConsole> StreamController::getConsoles() const {
    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
    return consoles_;
}

std::string StreamController::getConsoleFetchError() const {
    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
    return last_console_error_;
}

bool StreamController::hasCloudAccess() const {
    if (mock_mode_.load()) return true;
    return auth_ && auth_->hasCloudAccess();
}

bool StreamController::restoreCloudTitlesFromCache() {
    if (!loadCloudLibraryCache(true)) return false;

    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
    last_cloud_title_error_.clear();
    lunar::diagnosticLog("stream-controller",
                         "Restored cloud library cache count=%zu recent=%zu new=%zu",
                         cloud_titles_.size(),
                         recent_cloud_titles_.size(),
                         new_cloud_titles_.size());
    return !cloud_titles_.empty();
}

bool StreamController::fetchCloudTitles(bool force_refresh) {
    std::lock_guard<std::mutex> auth_lock(auth_operation_mutex_);
    auto signout_requested = [this]() {
        return signing_out_.load() ||
            lunar::platform::networkWorkersShuttingDown();
    };

    if (!force_refresh) {
        if (loadCloudLibraryCache()) {
            std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
            if (!cloud_titles_.empty()) {
                last_cloud_title_error_.clear();
                lunar::diagnosticLog("stream-controller",
                                     "Loaded cloud library from cache count=%zu recent=%zu new=%zu",
                                     cloud_titles_.size(),
                                     recent_cloud_titles_.size(),
                                     new_cloud_titles_.size());
                return true;
            }
        }
    }

    std::shared_ptr<api::XboxApiClient> api;
    if (mock_mode_.load()) {
        {
            std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
            api = api_;
        }
        if (!api) {
            std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
            last_cloud_title_error_ = "Mock mode is not initialized.";
            return false;
        }
        api->setSessionKind(api::GssvSessionKind::Cloud);
        if (!base_url_.empty()) {
            api->setBaseUrl(base_url_);
            api->setCatalogBaseUrl(base_url_);
        }
    } else {
    auto& auth_ref = auth();
    // If cloud still missing (e.g. previous InvalidCountry), force re-derive after region change.
    if (!auth_ref.hasCloudAccess() && !auth_ref.getForceRegionIp().empty()) {
        auth_ref.refreshStreamingTokens(true, signout_requested);
    }
    if (signout_requested() ||
        !auth_ref.refreshTokensIfNeeded(signout_requested) ||
        !auth_ref.isAuthenticated() ||
        signout_requested()) {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        cloud_titles_.clear();
        recent_cloud_titles_.clear();
        new_cloud_titles_.clear();
        last_cloud_title_error_ = signout_requested()
            ? "Signing out..."
            : (auth_ref.getLastError().empty()
                ? "Please sign in again."
                : auth_ref.getLastError());
        return false;
    }
    auth_ref.saveTokens(lunar::get_token_path());
    lunar::diagnosticLog("stream-controller",
                         "Cloud fetch auth ready cloud=%s home=%s",
                         auth_ref.hasCloudAccess() ? "yes" : "no",
                         auth_ref.getHomeStreamingToken().valid() ? "yes" : "no");

    if (!auth_ref.hasCloudAccess()) {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        cloud_titles_.clear();
        recent_cloud_titles_.clear();
        new_cloud_titles_.clear();
        last_cloud_title_error_ = auth_ref.getLastError().empty()
            ? "xCloud access denied by Xbox (HTTP 403). Game Pass Ultimate required, or region/subscription unavailable."
            : auth_ref.getLastError();
        return false;
    }

    api = makeApiClient(SessionType::Cloud);
    if (!api) {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        cloud_titles_.clear();
        recent_cloud_titles_.clear();
        new_cloud_titles_.clear();
        last_cloud_title_error_ = "Could not create xCloud API client.";
        return false;
    }
    } // !mock_mode

    api->setCatalogLanguage(getPreferredGameLanguage());

    std::vector<api::CloudTitle> recent;
    std::vector<api::CloudTitle> newly;
    auto titles = api->getHydratedCloudLibrary(&recent, &newly, signout_requested);
    std::string api_error = api->getLastError();
    if (titles.empty() && !mock_mode_.load()) {
        // Prefer showing a cached library over a blank screen after timeout.
        if (loadCloudLibraryCache(true)) {
            std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
            if (!cloud_titles_.empty()) {
                last_cloud_title_error_ =
                    api_error.empty()
                        ? "Showing cached xCloud library (live refresh timed out)."
                        : ("Showing cached library. Live refresh failed: " + api_error);
                lunar::diagnosticLog("stream-controller",
                                     "Using cache after live fetch miss count=%zu",
                                     cloud_titles_.size());
                return true;
            }
        }
    }
    if (signout_requested()) {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        cloud_titles_.clear();
        recent_cloud_titles_.clear();
        new_cloud_titles_.clear();
        last_cloud_title_error_ = "Signing out...";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        cloud_titles_ = std::move(titles);
        recent_cloud_titles_ = std::move(recent);
        new_cloud_titles_ = std::move(newly);
        last_cloud_title_error_ = cloud_titles_.empty()
            ? (api_error.empty() ? "No cloud titles found." : api_error)
            : "";
        lunar::diagnosticLog("stream-controller",
                             "Fetched cloud library count=%zu recent=%zu new=%zu",
                             cloud_titles_.size(),
                             recent_cloud_titles_.size(),
                             new_cloud_titles_.size());
        if (cloud_titles_.empty()) {
            return false;
        }
    }
    saveCloudLibraryCache();
    return true;
}

std::vector<api::CloudTitle> StreamController::getCloudTitles() const {
    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
    return cloud_titles_;
}

std::vector<api::CloudTitle> StreamController::getRecentCloudTitles() const {
    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
    return recent_cloud_titles_;
}

std::vector<api::CloudTitle> StreamController::getNewCloudTitles() const {
    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
    return new_cloud_titles_;
}

std::string StreamController::getCloudTitleFetchError() const {
    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
    return last_cloud_title_error_;
}

namespace {

constexpr long kMaxCloudLibraryCacheBytes = 16L * 1024L * 1024L;
constexpr long kMaxConsoleCacheBytes = 1024L * 1024L;

bool writeCacheAtomically(const std::string& path, const char* data) {
    if (!data) return false;
    const std::string temporary = path + ".tmp";
    const std::string backup = path + ".bak";

    FILE* output = std::fopen(temporary.c_str(), "wb");
    if (!output) return false;
    const size_t length = std::strlen(data);
    const bool wrote = std::fwrite(data, 1, length, output) == length;
    const bool closed = std::fclose(output) == 0;
    if (!wrote || !closed) {
        std::remove(temporary.c_str());
        return false;
    }

    bool had_existing = false;
    if (FILE* current = std::fopen(path.c_str(), "rb")) {
        had_existing = true;
        std::fclose(current);
    }
    std::remove(backup.c_str());
    if (had_existing && std::rename(path.c_str(), backup.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        if (had_existing) std::rename(backup.c_str(), path.c_str());
        std::remove(temporary.c_str());
        return false;
    }
    if (had_existing) std::remove(backup.c_str());
    return true;
}

void appendCloudTitleJson(cJSON* arr, const api::CloudTitle& title) {
    if (!arr) return;
    cJSON* obj = cJSON_CreateObject();
    if (!obj) return;
    cJSON_AddStringToObject(obj, "title_id", title.title_id.c_str());
    cJSON_AddStringToObject(obj, "product_id", title.product_id.c_str());
    cJSON_AddStringToObject(obj, "name", title.name.c_str());
    cJSON_AddStringToObject(obj, "image_url", title.image_url.c_str());
    cJSON_AddStringToObject(obj, "publisher", title.publisher.c_str());
    cJSON_AddBoolToObject(obj, "is_recent", title.is_recent);
    cJSON_AddItemToArray(arr, obj);
}

std::vector<api::CloudTitle> parseCloudTitleArray(cJSON* arr) {
    std::vector<api::CloudTitle> out;
    if (!arr || !cJSON_IsArray(arr)) return out;
    const int n = cJSON_GetArraySize(arr);
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        cJSON* item = cJSON_GetArrayItem(arr, i);
        if (!item) continue;
        api::CloudTitle title;
        cJSON* v;
        if ((v = cJSON_GetObjectItem(item, "title_id")) && cJSON_IsString(v) && v->valuestring)
            title.title_id = v->valuestring;
        if ((v = cJSON_GetObjectItem(item, "product_id")) && cJSON_IsString(v) && v->valuestring)
            title.product_id = v->valuestring;
        if ((v = cJSON_GetObjectItem(item, "name")) && cJSON_IsString(v) && v->valuestring)
            title.name = v->valuestring;
        if ((v = cJSON_GetObjectItem(item, "image_url")) && cJSON_IsString(v) && v->valuestring)
            title.image_url = v->valuestring;
        if ((v = cJSON_GetObjectItem(item, "publisher")) && cJSON_IsString(v) && v->valuestring)
            title.publisher = v->valuestring;
        if ((v = cJSON_GetObjectItem(item, "is_recent")) && cJSON_IsBool(v))
            title.is_recent = cJSON_IsTrue(v);
        if (!title.title_id.empty()) out.push_back(std::move(title));
    }
    return out;
}

} // namespace

bool StreamController::loadCloudLibraryCache(bool allow_stale) {
    const std::string expected_locale = getPreferredGameLanguage();
    const char* path = lunar::get_cloud_library_cache_path();
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    long len = std::ftell(f);
    if (len <= 0 || len > kMaxCloudLibraryCacheBytes) {
        std::fclose(f);
        return false;
    }
    std::rewind(f);
    std::string content(static_cast<size_t>(len), '\0');
    if (std::fread(content.data(), 1, static_cast<size_t>(len), f) != static_cast<size_t>(len)) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) return false;
    cJSON* ts = cJSON_GetObjectItem(root, "cache_time_ms");
    if (!ts || !cJSON_IsNumber(ts)) {
        cJSON_Delete(root);
        return false;
    }
    cJSON* locale = cJSON_GetObjectItem(root, "locale");
    const std::string cached_locale =
        locale && cJSON_IsString(locale) && locale->valuestring
            ? locale->valuestring
            : "en-US";
    if (cached_locale != expected_locale) {
        cJSON_Delete(root);
        return false;
    }
    const auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const auto cache_ms = static_cast<uint64_t>(ts->valuedouble);
    // A fresh cache can satisfy fetchCloudTitles(false). The UI may explicitly
    // restore an older cache for immediate browsing until the user refreshes it.
    constexpr uint64_t kMaxAgeMs = 7ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
    const bool stale = now_ms < cache_ms || now_ms - cache_ms > kMaxAgeMs;
    if (stale && !allow_stale) {
        cJSON_Delete(root);
        return false;
    }

    auto library = parseCloudTitleArray(cJSON_GetObjectItem(root, "library"));
    auto recent = parseCloudTitleArray(cJSON_GetObjectItem(root, "recent"));
    auto newly = parseCloudTitleArray(cJSON_GetObjectItem(root, "new_titles"));
    cJSON_Delete(root);
    if (library.empty()) return false;

    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
    cloud_titles_ = std::move(library);
    recent_cloud_titles_ = std::move(recent);
    new_cloud_titles_ = std::move(newly);
    lunar::diagnosticLog("stream-controller",
                         "Cloud library cache loaded stale=%s allow_stale=%s",
                         stale ? "true" : "false",
                         allow_stale ? "true" : "false");
    return true;
}

void StreamController::saveCloudLibraryCache() const {
    std::vector<api::CloudTitle> library;
    std::vector<api::CloudTitle> recent;
    std::vector<api::CloudTitle> newly;
    std::string locale;
    {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        library = cloud_titles_;
        recent = recent_cloud_titles_;
        newly = new_cloud_titles_;
        locale = preferred_game_language_;
    }
    if (library.empty()) return;

    cJSON* root = cJSON_CreateObject();
    if (!root) return;
    const auto now_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    cJSON_AddNumberToObject(root, "cache_time_ms", now_ms);
    cJSON_AddStringToObject(root, "locale", locale.c_str());
    cJSON* lib_arr = cJSON_CreateArray();
    cJSON* recent_arr = cJSON_CreateArray();
    cJSON* new_arr = cJSON_CreateArray();
    if (!lib_arr || !recent_arr || !new_arr) {
        cJSON_Delete(lib_arr);
        cJSON_Delete(recent_arr);
        cJSON_Delete(new_arr);
        cJSON_Delete(root);
        return;
    }
    for (const auto& t : library) appendCloudTitleJson(lib_arr, t);
    for (const auto& t : recent) appendCloudTitleJson(recent_arr, t);
    for (const auto& t : newly) appendCloudTitleJson(new_arr, t);
    cJSON_AddItemToObject(root, "library", lib_arr);
    cJSON_AddItemToObject(root, "recent", recent_arr);
    cJSON_AddItemToObject(root, "new_titles", new_arr);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;
    if (writeCacheAtomically(lunar::get_cloud_library_cache_path(), json)) {
        lunar::diagnosticLog("stream-controller", "Saved cloud library cache path=%s",
                             lunar::get_cloud_library_cache_path());
    }
    free(json);
}


bool StreamController::loadConsoleCache() {
    const char* path = lunar::get_xbox_console_cache_path();
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    long len = std::ftell(f);
    if (len <= 0 || len > kMaxConsoleCacheBytes) {
        std::fclose(f);
        return false;
    }
    std::rewind(f);
    std::string content(static_cast<size_t>(len), '\0');
    if (std::fread(content.data(), 1, static_cast<size_t>(len), f) != static_cast<size_t>(len)) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) return false;
    cJSON* arr = cJSON_GetObjectItem(root, "consoles");
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(root); return false; }

    std::vector<api::XboxConsole> cached;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        api::XboxConsole c;
        cJSON* jid = cJSON_GetObjectItem(item, "id");
        cJSON* jname = cJSON_GetObjectItem(item, "name");
        cJSON* jtype = cJSON_GetObjectItem(item, "console_type");
        cJSON* jpower = cJSON_GetObjectItem(item, "power_state");
        if (jid && cJSON_IsString(jid)) c.id = jid->valuestring;
        if (jname && cJSON_IsString(jname)) c.name = jname->valuestring;
        if (jtype && cJSON_IsString(jtype)) c.console_type = jtype->valuestring;
        if (jpower && cJSON_IsString(jpower)) c.power_state = jpower->valuestring;
        cached.push_back(std::move(c));
    }
    cJSON_Delete(root);

    if (cached.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        consoles_ = std::move(cached);
    }
    lunar::diagnosticLog("stream-controller", "Loaded console cache count=%zu", consoles_.size());
    return true;
}

void StreamController::saveConsoleCache() const {
    std::vector<api::XboxConsole> copy;
    {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        copy = consoles_;
    }
    if (copy.empty()) return;

    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return;
    }
    for (const auto& c : copy) {
        cJSON* entry = cJSON_CreateObject();
        if (!entry) continue;
        cJSON_AddStringToObject(entry, "id", c.id.c_str());
        cJSON_AddStringToObject(entry, "name", c.name.c_str());
        cJSON_AddStringToObject(entry, "console_type", c.console_type.c_str());
        cJSON_AddStringToObject(entry, "power_state", c.power_state.c_str());
        cJSON_AddItemToObject(arr, "", entry);
    }
    cJSON_AddItemToObject(root, "consoles", arr);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;
    if (writeCacheAtomically(lunar::get_xbox_console_cache_path(), json)) {
        lunar::diagnosticLog("stream-controller", "Saved console cache count=%zu",
                             copy.size());
    }
    free(json);
}

std::string StreamController::getLastStreamError() const {
    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
    return last_stream_error_;
}

void StreamController::requestGuideButton() {
    guide_button_requested_.store(true);
}

void StreamController::requestPlatformHomeButton() {
    requestGuideButton();
}

bool StreamController::consumeGuideButtonRequest() {
    return guide_button_requested_.exchange(false);
}

bool StreamController::isStreamCancelled(uint32_t generation) const {
    return cancel_requested_.load() || stream_generation_.load() != generation;
}

void StreamController::requestStreamStop() {
    cancel_requested_ = true;
    stream_generation_.fetch_add(1);
}

void StreamController::cleanupStreamResources(bool set_disconnected) {
    {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        streaming_ = false;
        input_router_.setOwner(input::StreamInputOwner::Game);
        guide_button_requested_ = false;
        session_id_.clear();
    }

    if (stream_session_) {
        stream_session_->stop(true);
    } else {
        if (transport_) {
            transport_->disconnect();
        }
        if (media_) {
            media_->shutdown();
        }
        if (rumble_) {
            rumble_->stop();
        }
    }

    if (set_disconnected) {
        setState(StreamState::Disconnected);
    }
}

bool StreamController::startStream(const std::string& server_id,
                                   int width,
                                   int height) {
    return startStream(server_id, width, height, stream::MediaPipelineOptions{});
}

bool StreamController::startStream(const std::string& server_id,
                                   int width,
                                   int height,
                                   const stream::MediaPipelineOptions& options) {
    return startStream(server_id, width, height, options, 0);
}

bool StreamController::startStream(const std::string& server_id,
                                   int width,
                                   int height,
                                   const stream::MediaPipelineOptions& options,
                                   int bitrate_kbps) {
    StreamProfile profile = makeHomeStreamProfile(
        server_id, width, height, bitrate_kbps);
    return startStreamWithProfile(profile, options);
}

bool StreamController::startCloudStream(const std::string& title_id,
                                        int width,
                                        int height) {
    return startCloudStream(title_id, width, height, stream::MediaPipelineOptions{});
}

bool StreamController::startCloudStream(const std::string& title_id,
                                        int width,
                                        int height,
                                        const stream::MediaPipelineOptions& options) {
    return startCloudStream(title_id, width, height, options, 0);
}

bool StreamController::startCloudStream(const std::string& title_id,
                                        int width,
                                        int height,
                                        const stream::MediaPipelineOptions& options,
                                        int bitrate_kbps) {
    StreamProfile profile = makeCloudStreamProfile(
        title_id, width, height, bitrate_kbps);
    profile.locale = getPreferredGameLanguage();
    return startStreamWithProfile(profile, options);
}

std::shared_ptr<api::XboxApiClient> StreamController::makeApiClient(SessionType type) {
    if (mock_mode_.load()) {
        std::shared_ptr<api::XboxApiClient> api;
        {
            std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
            api = api_;
        }
        if (api) {
            api->setSessionKind(type == SessionType::Cloud
                ? api::GssvSessionKind::Cloud
                : api::GssvSessionKind::Home);
            if (!base_url_.empty()) {
                api->setBaseUrl(base_url_);
                api->setCatalogBaseUrl(base_url_);
            }
        }
        return api;
    }

    auto& auth_ref = auth();
    const bool use_cloud = type == SessionType::Cloud;
    auth::StreamingToken streaming = use_cloud
        ? auth_ref.getCloudStreamingToken()
        : auth_ref.getHomeStreamingToken();
    if (!streaming.valid()) {
        streaming.gs_token = auth_ref.getGssvToken();
    }
    if (streaming.gs_token.empty()) {
        return nullptr;
    }

    auto api = std::make_shared<api::XboxApiClient>(
        http(),
        auth_ref.getWebToken(),
        auth_ref.getUserHash(),
        streaming.gs_token);
    api->setSessionKind(use_cloud ? api::GssvSessionKind::Cloud
                                  : api::GssvSessionKind::Home);

    if (!base_url_.empty() && !use_cloud) {
        api->setBaseUrl(base_url_);
    } else if (!streaming.base_uri.empty()) {
        api->setBaseUrl(streaming.base_uri);
    } else if (use_cloud) {
        api->setBaseUrl(api::constants::GSSV_CLOUD_BASE);
    }
    return api;
}

bool StreamController::startStreamWithProfile(
    const StreamProfile& input_profile,
    const stream::MediaPipelineOptions& options,
    CancelCallback cancel) {
    StreamProfile profile = input_profile;
    const XboxIcePreference ice_preference = XboxIcePreferenceStore().load(
        profile.type == SessionType::Home ? profile.server_id : std::string{});
    profile.preferred_stun_url = ice_preference.preferred_stun_url;
    if (profile.type == SessionType::Home && ice_preference.hasHomeRoute()) {
        profile.preferred_remote_ice_address = ice_preference.remote_address;
        profile.preferred_remote_ice_port = ice_preference.remote_port;
    }
    stream::MediaPipelineOptions xbox_options = options;
    xbox_options.video_scheduling =
        stream::VideoSchedulingMode::RealtimeQueued;
    {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        active_profile_ = profile;
        active_media_options_ = xbox_options;
        has_active_profile_ = true;
    }
    lunar::diagnosticLog(
        "stream-controller",
        "Start stream begin type=%s target=%s width=%d height=%d backend=%s",
        profile.type == SessionType::Cloud ? "cloud" : "home",
        profile.type == SessionType::Cloud ? profile.title_id.c_str()
                                           : profile.server_id.c_str(),
        profile.width,
        profile.height,
        stream::videoBackendName(options.video_backend));
    requestStreamStop();

    std::lock_guard<std::mutex> operation_lock(stream_operation_mutex_);
    if (cancel && cancel()) return false;
    if (signing_out_.load()) {
        return false;
    }
    const uint32_t generation = stream_generation_.fetch_add(1) + 1;
    cancel_requested_ = false;
    auto stream_cancel = [this, generation, cancel]() {
        return isStreamCancelled(generation) || signing_out_.load() ||
            (cancel && cancel());
    };
    if (cancel && cancel()) {
        requestStreamStop();
        return false;
    }
    input_router_.setOwner(input::StreamInputOwner::Game);
    guide_button_requested_ = false;

    cleanupStreamResources(false);
    stream_session_.reset();
    session_client_.reset();
    channels_.reset();
    ensureStreamingComponents();

    if (isStreamCancelled(generation) || signing_out_.load()) {
        return false;
    }

    if (!mock_mode_.load()) {
        auto& auth_ref = auth();
        if (!auth_ref.refreshTokensIfNeeded(stream_cancel) ||
            !auth_ref.isAuthenticated()) {
            if (stream_cancel()) return false;
            {
                std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
                last_stream_error_ = auth_ref.getLastError().empty()
                    ? "Sign-in expired. Please sign in again."
                    : auth_ref.getLastError();
            }
            setState(StreamState::Error, getLastStreamError());
            return false;
        }
        auth_ref.saveTokens(lunar::get_token_path());

        if (profile.type == SessionType::Cloud) {
            if (!auth_ref.hasCloudAccess()) {
                {
                    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
                    last_stream_error_ =
                        (auth_ref.getLastError().empty()
                            ? "xCloud access denied by Xbox (HTTP 403). Game Pass Ultimate required, or region/subscription unavailable."
                            : auth_ref.getLastError());
                }
                setState(StreamState::Error, getLastStreamError());
                return false;
            }
            // Must be XStreaming lpt (cloud transfer token), not xboxlive.signin access token.
            profile.msal_user_token =
                auth_ref.getXcloudTransferToken(true, stream_cancel);
            if (profile.msal_user_token.empty()) {
                if (stream_cancel()) return false;
                {
                    std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
                    last_stream_error_ = auth_ref.getLastError().empty()
                        ? "Failed to get xCloud connect token (lpt)."
                        : auth_ref.getLastError();
                }
                setState(StreamState::Error, getLastStreamError());
                return false;
            }
            const auto cloud = auth_ref.getCloudStreamingToken();
            if (profile.base_url.empty()) {
                profile.base_url = cloud.base_uri;
            }
        } else {
            const auto home = auth_ref.getHomeStreamingToken();
            if (profile.base_url.empty() && !home.base_uri.empty()) {
                profile.base_url = home.base_uri;
            }
        }
    } else if (profile.type == SessionType::Cloud) {
        // mock cloud ReadyToConnect path
        if (profile.msal_user_token.empty()) {
            profile.msal_user_token = "mock-msal-token";
        }
        if (profile.base_url.empty() && !base_url_.empty()) {
            profile.base_url = base_url_;
        }
    }

    auto api = makeApiClient(profile.type);
    if (!api) {
        {
            std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
            last_stream_error_ = profile.type == SessionType::Cloud
                ? "No xCloud session is available. Refresh cloud titles after sign-in."
                : "No console session is available. Refresh your Xbox list.";
        }
        lunar::diagnosticLog("stream-controller", "Start stream failed: %s",
                             getLastStreamError().c_str());
        setState(StreamState::Error, getLastStreamError());
        return false;
    }
    if (!profile.base_url.empty()) {
        api->setBaseUrl(profile.base_url);
    }

    {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        api_ = api;
        active_session_type_ = profile.type;
        session_id_.clear();
    }

    setState(StreamState::Connecting, "");

    transport_ = std::make_unique<WebRtcTransport>(
        std::make_unique<webrtc::PeerManager>());
    channels_ = std::make_unique<XboxChannelManager>(*transport_);
    session_client_ = std::make_unique<XboxSessionClient>(api);
    stream_session_ = std::make_unique<XboxStreamSession>(
        *session_client_,
        *transport_,
        *channels_,
        *media_,
        *gamepad_,
        *xinput_,
        *rumble_,
        input_router_,
        perf_);

    XboxStreamSession::RuntimeCallbacks callbacks;
    callbacks.external_cancel = stream_cancel;
    callbacks.consume_guide_button = [this]() {
        return consumeGuideButtonRequest();
    };
    callbacks.on_status = [this, generation](const std::string& info) {
        if (!isStreamCancelled(generation) && !streaming_.load()) {
            setState(StreamState::Connecting, info);
        }
    };
    callbacks.on_session_id = [this](const std::string& session_id) {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        session_id_ = session_id;
    };
    callbacks.on_streaming = [this, width = profile.width, height = profile.height]() {
        stream_width_ = width;
        stream_height_ = height;
        streaming_ = true;
        setState(StreamState::Streaming, "");
    };
    callbacks.on_cancelled = [this](const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
            streaming_ = false;
            session_id_.clear();
            last_stream_error_ = reason;
        }
        lunar::diagnosticLog("stream-controller", "Stream cancelled: %s", reason.c_str());
        setState(StreamState::Disconnected, reason);
    };
    callbacks.on_error = [this](const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
            streaming_ = false;
            session_id_.clear();
            last_stream_error_ = reason;
        }
        lunar::diagnosticLog("stream-controller", "Stream error: %s", reason.c_str());
        setState(StreamState::Error, reason);
    };
    callbacks.refresh_tokens = [this](
                                   bool force,
                                   XboxStreamSession::RuntimeCallbacks::CancelCallback cancel) {
        if (mock_mode_.load()) {
            return true;
        }
        auto& auth_ref = auth();
        const bool refreshed = force
            ? auth_ref.refreshStreamingTokens(true, cancel)
            : auth_ref.refreshTokensIfNeeded(cancel);
        if (!refreshed || (cancel && cancel())) {
            return false;
        }
        auth_ref.saveTokens(lunar::get_token_path());
        if (session_client_) {
            const SessionType type = active_session_type_;
            const auto streaming = type == SessionType::Cloud
                ? auth_ref.getCloudStreamingToken()
                : auth_ref.getHomeStreamingToken();
            const std::string gs = streaming.valid() ? streaming.gs_token
                                                     : auth_ref.getGssvToken();
            session_client_->updateTokens(auth_ref.getWebToken(), gs);
        }
        return true;
    };

    return stream_session_->start(profile, xbox_options, std::move(callbacks));
}

void StreamController::stopStream() {
    stopStream(true);
}

void StreamController::requestStop() {
    requestStreamStop();
}

void StreamController::stopStream(bool set_disconnected) {
    const auto stop_started_at = std::chrono::steady_clock::now();
    lunar::persistentEventLog("stream-controller",
                              "stop stream begin set_disconnected=%s",
                              set_disconnected ? "true" : "false");
    requestStreamStop();
    const auto lock_started_at = std::chrono::steady_clock::now();
    lunar::persistentEventLog("stream-controller",
                              "stop phase=operation-lock begin");
    std::lock_guard<std::mutex> operation_lock(stream_operation_mutex_);
    const auto lock_wait = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - lock_started_at);
    lunar::persistentEventLog(
        "stream-controller",
        "stop phase=operation-lock done wait_ms=%lld slow=%s",
        static_cast<long long>(lock_wait.count()),
        lock_wait >= std::chrono::seconds(3) ? "true" : "false");
    cleanupStreamResources(set_disconnected);
    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stop_started_at);
    lunar::persistentEventLog(
        "stream-controller", "stop stream complete total_ms=%lld slow=%s",
        static_cast<long long>(total.count()),
        total >= std::chrono::seconds(3) ? "true" : "false");
}

bool StreamController::resumeAfterForeground(
    app::IStreamRuntime::CancelCallback cancel) {
    if (cancel && cancel()) return false;
    {
        std::lock_guard<std::mutex> operation_lock(stream_operation_mutex_);
        if (cancel && cancel()) return false;
        if (state_.load() == StreamState::Streaming && transport_ &&
            transport_->isConnected()) {
            if (media_) {
                media_->requestVideoRecovery("foreground resume", true);
            }
            lunar::diagnosticLog("stream-controller",
                                 "foreground resume kept healthy session");
            return true;
        }
    }

    StreamProfile profile;
    stream::MediaPipelineOptions options;
    {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        if (!has_active_profile_) {
            lunar::diagnosticLog("stream-controller",
                                 "foreground resume has no saved profile");
            return false;
        }
        profile = active_profile_;
        options = active_media_options_;
    }
    if (cancel && cancel()) return false;
    lunar::diagnosticLog("stream-controller",
                         "foreground resume rebuilding Xbox session");
    return startStreamWithProfile(profile, options, std::move(cancel));
}

void StreamController::update() {
    if (state_.load() != StreamState::Streaming) {
        return;
    }
    if (rumble_) {
        rumble_->update();
    }
}

void StreamController::presentVideoFrame() {
    if (media_) {
        media_->presentVideoFrame();
    }
}

void StreamController::setVideoPresentationSuspended(bool suspended) {
    if (media_) {
        media_->setVideoPresentationSuspended(suspended);
    }
}

bool StreamController::bypassAuthForMock(const std::string& base_url) {
    base_url_ = base_url;
    mock_mode_ = true;

    // Create a dummy API client with fake tokens (mock server doesn't validate)
    auto api = std::make_shared<api::XboxApiClient>(
        http(), "mock_web_token", "mock_user_hash", "mock_gssv_token");
    api->setBaseUrl(base_url_);
    api->setCatalogBaseUrl(base_url_);

    // Store it so fetchConsoles / startStream will use it
    {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        api_ = std::move(api);
    }

    // Populate a mock console so the UI has something to connect to
    {
        std::lock_guard<std::mutex> lock(stream_lifecycle_mutex_);
        api::XboxConsole mock_console;
        mock_console.id = "MOCKXBOX001";
        mock_console.name = "Mock-Xbox-Series-X";
        mock_console.console_type = "XboxSeriesX";
        mock_console.power_state = "On";
        consoles_ = {mock_console};
    }

    setState(StreamState::Disconnected, "Mock mode ready");
    return true;
}

} // namespace lunar::app
