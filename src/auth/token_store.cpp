#include "token_store.h"
#include <cJSON.h>
#include <cstdio>
#include <chrono>

namespace lunar::auth {

TokenStore::TokenStore() = default;
TokenStore::~TokenStore() = default;

// =============================================================================
// Helper: write a string if non-empty
// =============================================================================
template<typename F>
static void set_str(F& field, cJSON* obj, const char* key) {
    if (cJSON* item = cJSON_GetObjectItem(obj, key)) {
        if (cJSON_IsString(item)) field = item->valuestring;
    }
}

template<typename F>
static void set_int(F& field, cJSON* obj, const char* key) {
    if (cJSON* item = cJSON_GetObjectItem(obj, key)) {
        if (cJSON_IsNumber(item)) field = static_cast<F>(item->valueint);
    }
}

template<typename F>
static void set_uint64(F& field, cJSON* obj, const char* key) {
    if (cJSON* item = cJSON_GetObjectItem(obj, key)) {
        if (cJSON_IsNumber(item)) field = static_cast<uint64_t>(item->valuedouble);
    }
}

// =============================================================================
// Load
// =============================================================================
bool TokenStore::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    static constexpr long kMaxTokenFileSize = 65536;
    if (len <= 0 || len > kMaxTokenFileSize) {
        fclose(f);
        return false;
    }

    std::string content(len, '\0');
    fread(&content[0], 1, len, f);
    fclose(f);

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) return false;

    // User token
    set_str(data_.access_token, root, "access_token");
    set_str(data_.refresh_token, root, "refresh_token");
    set_str(data_.user_id, root, "user_id");
    set_str(data_.expires_on, root, "expires_on");
    set_int(data_.expires_in, root, "expires_in");
    set_uint64(data_.expires_at_ms, root, "expires_at_ms");

    // Sisu tokens
    cJSON* sisu = cJSON_GetObjectItem(root, "sisuToken");
    if (sisu) {
        set_str(data_.sisu.device_token, sisu, "device_token");
        set_str(data_.sisu.title_token, sisu, "title_token");
        set_str(data_.sisu.title_token_expires, sisu, "title_token_expires");
        set_str(data_.sisu.user_token, sisu, "user_token");
        set_str(data_.sisu.user_token_expires, sisu, "user_token_expires");
        set_str(data_.sisu.authorization_token, sisu, "authorization_token");
        set_str(data_.sisu.authorization_token_expires, sisu, "authorization_token_expires");
        set_str(data_.sisu.user_hash, sisu, "user_hash");
        set_str(data_.sisu.gamertag, sisu, "gamertag");
    }

    // JWK keys
    set_str(data_.jwk_x, root, "jwk_x");
    set_str(data_.jwk_y, root, "jwk_y");
    set_str(data_.jwk_d, root, "jwk_d");

    // Streaming tokens
    set_str(data_.gssv_token, root, "gssv_token");
    set_str(data_.gssv_base_uri, root, "gssv_base_uri");
    set_int(data_.gssv_duration_seconds, root, "gssv_duration_seconds");
    set_str(data_.gssv_cloud_token, root, "gssv_cloud_token");
    set_str(data_.gssv_cloud_base_uri, root, "gssv_cloud_base_uri");
    set_int(data_.gssv_cloud_duration_seconds, root, "gssv_cloud_duration_seconds");
    set_str(data_.web_token, root, "web_token");
    set_str(data_.gamertag, root, "gamertag");
    set_str(data_.user_hash, root, "user_hash");

    // Timestamp
    set_uint64(data_.token_update_time, root, "token_update_time");

    cJSON_Delete(root);
    return true;
}

// =============================================================================
// Save
// =============================================================================
bool TokenStore::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject();

    // User token
    if (!data_.access_token.empty()) cJSON_AddStringToObject(root, "access_token", data_.access_token.c_str());
    if (!data_.refresh_token.empty()) cJSON_AddStringToObject(root, "refresh_token", data_.refresh_token.c_str());
    if (!data_.user_id.empty()) cJSON_AddStringToObject(root, "user_id", data_.user_id.c_str());
    if (!data_.expires_on.empty()) cJSON_AddStringToObject(root, "expires_on", data_.expires_on.c_str());
    cJSON_AddNumberToObject(root, "expires_in", data_.expires_in);
    cJSON_AddNumberToObject(root, "expires_at_ms", static_cast<double>(data_.expires_at_ms));

    // Sisu tokens
    if (!data_.sisu.device_token.empty()) {
        cJSON* sisu = cJSON_CreateObject();
        cJSON_AddStringToObject(sisu, "device_token", data_.sisu.device_token.c_str());
        cJSON_AddStringToObject(sisu, "title_token", data_.sisu.title_token.c_str());
        cJSON_AddStringToObject(sisu, "title_token_expires", data_.sisu.title_token_expires.c_str());
        cJSON_AddStringToObject(sisu, "user_token", data_.sisu.user_token.c_str());
        cJSON_AddStringToObject(sisu, "user_token_expires", data_.sisu.user_token_expires.c_str());
        cJSON_AddStringToObject(sisu, "authorization_token", data_.sisu.authorization_token.c_str());
        cJSON_AddStringToObject(sisu, "authorization_token_expires", data_.sisu.authorization_token_expires.c_str());
        cJSON_AddStringToObject(sisu, "user_hash", data_.sisu.user_hash.c_str());
        cJSON_AddStringToObject(sisu, "gamertag", data_.sisu.gamertag.c_str());
        cJSON_AddItemToObject(root, "sisuToken", sisu);
    }

    // JWK keys
    if (!data_.jwk_x.empty()) cJSON_AddStringToObject(root, "jwk_x", data_.jwk_x.c_str());
    if (!data_.jwk_y.empty()) cJSON_AddStringToObject(root, "jwk_y", data_.jwk_y.c_str());
    if (!data_.jwk_d.empty()) cJSON_AddStringToObject(root, "jwk_d", data_.jwk_d.c_str());

    // Streaming tokens
    if (!data_.gssv_token.empty()) cJSON_AddStringToObject(root, "gssv_token", data_.gssv_token.c_str());
    if (!data_.gssv_base_uri.empty()) cJSON_AddStringToObject(root, "gssv_base_uri", data_.gssv_base_uri.c_str());
    if (data_.gssv_duration_seconds > 0) cJSON_AddNumberToObject(root, "gssv_duration_seconds", data_.gssv_duration_seconds);
    if (!data_.gssv_cloud_token.empty()) cJSON_AddStringToObject(root, "gssv_cloud_token", data_.gssv_cloud_token.c_str());
    if (!data_.gssv_cloud_base_uri.empty()) cJSON_AddStringToObject(root, "gssv_cloud_base_uri", data_.gssv_cloud_base_uri.c_str());
    if (data_.gssv_cloud_duration_seconds > 0) cJSON_AddNumberToObject(root, "gssv_cloud_duration_seconds", data_.gssv_cloud_duration_seconds);
    if (!data_.web_token.empty()) cJSON_AddStringToObject(root, "web_token", data_.web_token.c_str());
    if (!data_.gamertag.empty()) cJSON_AddStringToObject(root, "gamertag", data_.gamertag.c_str());
    if (!data_.user_hash.empty()) cJSON_AddStringToObject(root, "user_hash", data_.user_hash.c_str());

    // Timestamp
    cJSON_AddNumberToObject(root, "token_update_time",
        static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));

    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);

    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        free(json_str);
        return false;
    }
    fputs(json_str, f);
    fclose(f);
    free(json_str);
    return true;
}

void TokenStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_ = TokenStoreData{};
}

bool TokenStore::hasValidUserToken() const {
    if (data_.access_token.empty() || data_.refresh_token.empty()) return false;
    if (data_.expires_at_ms == 0) return true;
    auto now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    return data_.expires_at_ms > now_ms + 60'000;
}

bool TokenStore::hasValidSisuToken() const {
    return !data_.sisu.authorization_token.empty();
}

void TokenStore::setUserToken(const std::string& access_token,
                               const std::string& refresh_token,
                               const std::string& user_id,
                               int expires_in) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.access_token = access_token;
    data_.refresh_token = refresh_token;
    data_.user_id = user_id;
    data_.expires_in = expires_in;
    auto now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    data_.expires_at_ms = now_ms + static_cast<uint64_t>(expires_in > 0 ? expires_in : 3600) * 1000ULL;
    data_.token_issue_steady_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void TokenStore::setSisuToken(const SisuTokenData& sisu) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.sisu = sisu;
}
} // namespace lunar::auth
