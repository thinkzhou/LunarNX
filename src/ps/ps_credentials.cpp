#if defined(__SWITCH__) || defined(LUNARNX_DESKTOP_TEST)

#include "ps_credentials.h"
#include <algorithm>
#include <cJSON.h>
#include <cstdio>
#include <cstring>

namespace lunar::ps {
namespace {

std::string jsonString(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : std::string{};
}

int jsonInt(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valueint : 0;
}

bool decodeHex(const std::string& text, uint8_t* out, size_t size) {
    if (text.size() != size * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < size; ++i) {
        int high = nibble(text[i * 2]);
        int low = nibble(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

cJSON* encodeHex(const uint8_t* data, size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string value(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        value[i * 2] = kHex[data[i] >> 4];
        value[i * 2 + 1] = kHex[data[i] & 0xf];
    }
    return cJSON_CreateString(value.c_str());
}

bool readFile(const std::string& path, std::string& content) {
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (length <= 0 || length > 65536) {
        std::fclose(file);
        return false;
    }
    content.resize(static_cast<size_t>(length));
    bool ok = std::fread(content.data(), 1, content.size(), file) == content.size();
    std::fclose(file);
    return ok;
}

bool validCredentialKeys(const RegisteredCredential& cred) {
    bool regist_nonzero = false;
    bool rp_nonzero = false;
    for (uint8_t byte : cred.rp_regist_key) regist_nonzero |= byte != 0;
    for (uint8_t byte : cred.rp_key) rp_nonzero |= byte != 0;
    return regist_nonzero && rp_nonzero;
}

bool saveHosts(const std::vector<RegisteredCredential>& hosts,
               const std::string& path) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON* array = cJSON_AddArrayToObject(root, "registered_hosts");
    for (const auto& cred : hosts) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "server_mac", cred.server_mac.c_str());
        cJSON_AddStringToObject(item, "nickname", cred.nickname.c_str());
        cJSON_AddStringToObject(item, "last_known_addr", cred.last_known_addr.c_str());
        if (!cred.psn_duid.empty()) {
            cJSON_AddStringToObject(item, "psn_duid", cred.psn_duid.c_str());
        }
        cJSON_AddNumberToObject(item, "target", cred.target);
        cJSON_AddItemToObject(item, "rp_regist_key",
                              encodeHex(cred.rp_regist_key, sizeof(cred.rp_regist_key)));
        cJSON_AddNumberToObject(item, "rp_key_type", cred.rp_key_type);
        cJSON_AddItemToObject(item, "rp_key",
                              encodeHex(cred.rp_key, sizeof(cred.rp_key)));
        if (!cred.console_login_pin.empty()) {
            cJSON_AddStringToObject(item, "console_login_pin",
                                    cred.console_login_pin.c_str());
        }
        cJSON_AddItemToArray(array, item);
    }

    char* json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return false;

    std::string temp_path = path + ".tmp";
    FILE* file = std::fopen(temp_path.c_str(), "w");
    if (!file) {
        std::free(json);
        return false;
    }
    bool ok = std::fputs(json, file) >= 0;
    ok = std::fflush(file) == 0 && ok;
    ok = std::fclose(file) == 0 && ok;
    std::free(json);
    if (!ok) {
        std::remove(temp_path.c_str());
        return false;
    }
    if (std::rename(temp_path.c_str(), path.c_str()) != 0) {
        std::remove(temp_path.c_str());
        return false;
    }
    return true;
}

} // namespace

bool PsCredentials::load(const std::string& path) {
    std::string content;
    if (!readFile(path, content)) return false;

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) return false;
    cJSON* array = cJSON_GetObjectItemCaseSensitive(root, "registered_hosts");
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(root);
        return false;
    }

    const int version = jsonInt(root, "version");
    std::vector<RegisteredCredential> loaded;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, array) {
        RegisteredCredential cred;
        std::string mac_text = version >= 2
            ? jsonString(item, "server_mac")
            : jsonString(item, "server_mac");
        auto mac = normalizeMac(mac_text);
        if (!mac) continue;

        cred.server_mac = *mac;
        cred.nickname = version >= 2
            ? jsonString(item, "nickname")
            : jsonString(item, "host_name");
        if (cred.nickname.empty()) cred.nickname = jsonString(item, "server_nickname");
        cred.last_known_addr = version >= 2
            ? jsonString(item, "last_known_addr")
            : jsonString(item, "host_addr");
        cred.target = jsonInt(item, "target");
        cred.rp_key_type = static_cast<uint32_t>(jsonInt(item, "rp_key_type"));
        if (!decodeHex(jsonString(item, "rp_regist_key"), cred.rp_regist_key,
                       sizeof(cred.rp_regist_key)) ||
            !decodeHex(jsonString(item, "rp_key"), cred.rp_key,
                       sizeof(cred.rp_key)) ||
            !validCredentialKeys(cred)) {
            continue;
        }

        if (version >= 2) {
            auto duid = normalizeDuid(jsonString(item, "psn_duid"));
            if (duid) cred.psn_duid = *duid;
            cred.console_login_pin = jsonString(item, "console_login_pin");
        }

        auto existing = std::find_if(loaded.begin(), loaded.end(),
            [&](const RegisteredCredential& value) {
                return value.server_mac == cred.server_mac;
            });
        if (existing == loaded.end()) loaded.push_back(std::move(cred));
        else *existing = std::move(cred);
    }
    cJSON_Delete(root);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        hosts_ = std::move(loaded);
    }
    if (version < 2) save(path);
    return true;
}

bool PsCredentials::save(const std::string& path) const {
    std::vector<RegisteredCredential> hosts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hosts = hosts_;
    }

    return saveHosts(hosts, path);
}

void PsCredentials::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    hosts_.clear();
}

std::vector<RegisteredCredential> PsCredentials::getRegisteredHosts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hosts_;
}

std::optional<RegisteredCredential> PsCredentials::findByMac(
    const std::string& server_mac) const {
    auto normalized = normalizeMac(server_mac);
    if (!normalized) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(hosts_.begin(), hosts_.end(),
        [&](const RegisteredCredential& cred) {
            return cred.server_mac == *normalized;
        });
    if (it == hosts_.end()) return std::nullopt;
    return *it;
}

bool PsCredentials::addAndSave(const RegisteredCredential& cred,
                               const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto updated = hosts_;
    auto it = std::find_if(updated.begin(), updated.end(),
        [&](const RegisteredCredential& value) {
            return value.server_mac == cred.server_mac;
        });
    if (it == updated.end()) updated.push_back(cred);
    else *it = cred;

    if (!saveHosts(updated, path)) return false;
    hosts_ = std::move(updated);
    return true;
}

bool PsCredentials::updateLastKnownAddrAndSave(const std::string& server_mac,
                                               const std::string& address,
                                               const std::string& path) {
    auto normalized = normalizeMac(server_mac);
    if (!normalized || address.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto updated = hosts_;
    auto it = std::find_if(updated.begin(), updated.end(),
        [&](const RegisteredCredential& value) {
            return value.server_mac == *normalized;
        });
    if (it == updated.end() || it->last_known_addr == address) return false;
    it->last_known_addr = address;
    if (!saveHosts(updated, path)) return false;
    hosts_ = std::move(updated);
    return true;
}

void PsCredentials::add(const RegisteredCredential& cred) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(hosts_.begin(), hosts_.end(),
        [&](const RegisteredCredential& value) {
            return value.server_mac == cred.server_mac;
        });
    if (it == hosts_.end()) hosts_.push_back(cred);
    else *it = cred;
}

void PsCredentials::remove(const std::string& server_mac) {
    auto normalized = normalizeMac(server_mac);
    if (!normalized) return;
    std::lock_guard<std::mutex> lock(mutex_);
    hosts_.erase(std::remove_if(hosts_.begin(), hosts_.end(),
        [&](const RegisteredCredential& cred) {
            return cred.server_mac == *normalized;
        }), hosts_.end());
}

bool PsCredentials::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hosts_.empty();
}

} // namespace lunar::ps

#endif
