#include "ps_route_preferences.h"

#include "../common.h"

#include <cJSON.h>

#include <cstdio>
#include <mutex>
#include <utility>

namespace lunar::ps {
namespace {

constexpr long kMaxPreferenceFileBytes = 256 * 1024;
constexpr int kMaxConsoleRoutes = 16;

std::mutex& preferenceFileMutex() {
    static std::mutex mutex;
    return mutex;
}

cJSON* readRoot(const std::string& path) {
    FILE* input = std::fopen(path.c_str(), "rb");
    if (!input) return nullptr;
    if (std::fseek(input, 0, SEEK_END) != 0) {
        std::fclose(input);
        return nullptr;
    }
    const long size = std::ftell(input);
    if (size <= 0 || size > kMaxPreferenceFileBytes ||
        std::fseek(input, 0, SEEK_SET) != 0) {
        std::fclose(input);
        return nullptr;
    }

    std::string json(static_cast<size_t>(size), '\0');
    const size_t read = std::fread(json.data(), 1, json.size(), input);
    std::fclose(input);
    if (read != json.size()) return nullptr;

    cJSON* root = cJSON_Parse(json.c_str());
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return nullptr;
    }
    return root;
}

std::string jsonString(cJSON* object, const char* name) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) && value->valuestring
        ? std::string(value->valuestring) : std::string{};
}

int jsonPort(cJSON* object, const char* name) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(value) && value->valueint > 0 &&
        value->valueint <= 65535 ? value->valueint : 0;
}

void replaceString(cJSON* object, const char* name, const std::string& value) {
    cJSON* replacement = cJSON_CreateString(value.c_str());
    if (cJSON_HasObjectItem(object, name)) {
        cJSON_ReplaceItemInObjectCaseSensitive(object, name, replacement);
    } else {
        cJSON_AddItemToObject(object, name, replacement);
    }
}

void replaceNumber(cJSON* object, const char* name, double value) {
    cJSON* replacement = cJSON_CreateNumber(value);
    if (cJSON_HasObjectItem(object, name)) {
        cJSON_ReplaceItemInObjectCaseSensitive(object, name, replacement);
    } else {
        cJSON_AddItemToObject(object, name, replacement);
    }
}

bool writeRoot(const std::string& path, cJSON* root) {
    char* json = cJSON_PrintUnformatted(root);
    if (!json) return false;

    const std::string temporary = path + ".tmp";
    FILE* output = std::fopen(temporary.c_str(), "wb");
    if (!output) {
        cJSON_free(json);
        return false;
    }
    const size_t length = std::char_traits<char>::length(json);
    const bool wrote = std::fwrite(json, 1, length, output) == length;
    const bool closed = std::fclose(output) == 0;
    cJSON_free(json);
    if (!wrote || !closed || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

} // namespace

std::string psRoutePreferenceKey(const uint8_t* console_uid, size_t size) {
    if (!console_uid || size == 0) return {};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string key(size * 2, '\0');
    for (size_t i = 0; i < size; ++i) {
        key[i * 2] = kHex[console_uid[i] >> 4];
        key[i * 2 + 1] = kHex[console_uid[i] & 0x0f];
    }
    return key;
}

PsRoutePreferenceStore::PsRoutePreferenceStore(std::string path)
    : path_(std::move(path)) {}

PsRoutePreferenceStore::PsRoutePreferenceStore()
    : path_(lunar::get_ps_route_preferences_path()) {}

PsRoutePreference PsRoutePreferenceStore::load(
    const std::string& console_key) const {
    std::lock_guard<std::mutex> lock(preferenceFileMutex());
    PsRoutePreference preference;
    cJSON* root = readRoot(path_);
    if (!root) return preference;

    preference.preferred_stun_host = jsonString(root, "preferredStunHost");
    preference.preferred_stun_port = jsonPort(root, "preferredStunPort");
    cJSON* routes = cJSON_GetObjectItemCaseSensitive(root, "consoleRoutes");
    if (cJSON_IsArray(routes) && !console_key.empty()) {
        const int count = cJSON_GetArraySize(routes);
        for (int i = 0; i < count; ++i) {
            cJSON* route = cJSON_GetArrayItem(routes, i);
            if (!cJSON_IsObject(route) ||
                jsonString(route, "consoleKey") != console_key) {
                continue;
            }
            preference.remote_address = jsonString(route, "remoteAddress");
            preference.remote_port = jsonPort(route, "remotePort");
            break;
        }
    }
    cJSON_Delete(root);

    if (!preference.hasPreferredStun()) {
        preference.preferred_stun_host.clear();
        preference.preferred_stun_port = 0;
    }
    if (!preference.hasRemoteRoute()) {
        preference.remote_address.clear();
        preference.remote_port = 0;
    }
    return preference;
}

bool PsRoutePreferenceStore::save(
    const std::string& console_key,
    const PsRoutePreference& preference) const {
    std::lock_guard<std::mutex> lock(preferenceFileMutex());
    cJSON* root = readRoot(path_);
    if (!root) root = cJSON_CreateObject();
    if (!root) return false;

    replaceNumber(root, "version", 1);
    if (preference.hasPreferredStun()) {
        replaceString(root, "preferredStunHost", preference.preferred_stun_host);
        replaceNumber(root, "preferredStunPort", preference.preferred_stun_port);
    }

    if (!console_key.empty() && preference.hasRemoteRoute()) {
        cJSON* routes = cJSON_GetObjectItemCaseSensitive(root, "consoleRoutes");
        if (!cJSON_IsArray(routes)) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, "consoleRoutes");
            routes = cJSON_AddArrayToObject(root, "consoleRoutes");
        }
        for (int i = cJSON_GetArraySize(routes) - 1; i >= 0; --i) {
            cJSON* route = cJSON_GetArrayItem(routes, i);
            if (cJSON_IsObject(route) &&
                jsonString(route, "consoleKey") == console_key) {
                cJSON_DeleteItemFromArray(routes, i);
            }
        }

        cJSON* route = cJSON_CreateObject();
        cJSON_AddStringToObject(route, "consoleKey", console_key.c_str());
        cJSON_AddStringToObject(
            route, "remoteAddress", preference.remote_address.c_str());
        cJSON_AddNumberToObject(route, "remotePort", preference.remote_port);
        cJSON_AddItemToArray(routes, route);
        while (cJSON_GetArraySize(routes) > kMaxConsoleRoutes) {
            cJSON_DeleteItemFromArray(routes, 0);
        }
    }

    const bool saved = writeRoot(path_, root);
    cJSON_Delete(root);
    return saved;
}

} // namespace lunar::ps
