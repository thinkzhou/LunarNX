#include "i18n.h"

#include "../common.h"
#include "../diagnostics.h"

#include <borealis.hpp>
#include <cJSON.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lunar::ui {
namespace {

constexpr long kMaxJsonBytes = 64 * 1024;

bool isSupportedLanguage(const std::string& language) {
    return language == "auto" || language == "en-US" ||
           language == "zh-Hans" || language == "zh-Hant";
}

cJSON* readJsonObject(const char* path) {
    FILE* input = std::fopen(path, "rb");
    if (!input) return nullptr;

    cJSON* root = nullptr;
    if (std::fseek(input, 0, SEEK_END) == 0) {
        const long length = std::ftell(input);
        if (length > 0 && length <= kMaxJsonBytes) {
            std::rewind(input);
            std::string data(static_cast<size_t>(length), '\0');
            if (std::fread(data.data(), 1, static_cast<size_t>(length), input) ==
                static_cast<size_t>(length)) {
                root = cJSON_Parse(data.c_str());
            }
        }
    }
    std::fclose(input);

    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return nullptr;
    }
    return root;
}

cJSON* readConfig() {
    cJSON* root = readJsonObject(lunar::get_config_path());
    return root ? root : cJSON_CreateObject();
}

std::string resolveNoticeLocale(const std::string& language) {
    if (language != "auto") {
        return isSupportedLanguage(language) ? language : "en-US";
    }

    uint64_t language_code = 0;
    if (R_SUCCEEDED(setGetSystemLanguage(&language_code))) {
        char language_name[sizeof(language_code) + 1] = {};
        std::memcpy(language_name, &language_code, sizeof(language_code));
        const std::string system_language(language_name);
        if (system_language == "zh-Hans" || system_language == "zh-Hant" ||
            system_language == "en-US") {
            return system_language;
        }
    }
    return "en-US";
}

bool writeConfig(cJSON* root) {
    if (!root) return false;
    char* json = cJSON_Print(root);
    if (!json) return false;

    bool written = false;
    FILE* output = std::fopen(lunar::get_config_path(), "wb");
    if (output) {
        written = std::fputs(json, output) >= 0;
        written = std::fclose(output) == 0 && written;
    }
    std::free(json);
    return written;
}

} // namespace

std::string getConfiguredLanguage() {
    cJSON* root = readConfig();
    if (!root) return "auto";

    std::string language = "auto";
    cJSON* value = cJSON_GetObjectItemCaseSensitive(root, "language");
    if (value && cJSON_IsString(value) && value->valuestring) {
        language = value->valuestring;
    }
    cJSON_Delete(root);
    return isSupportedLanguage(language) ? language : "auto";
}

std::string getResolvedAppLocale() {
    return resolveNoticeLocale(getConfiguredLanguage());
}

bool saveConfiguredLanguage(const std::string& language) {
    if (!isSupportedLanguage(language)) return false;

    cJSON* root = readConfig();
    if (!root) return false;
    cJSON_DeleteItemFromObjectCaseSensitive(root, "language");
    cJSON_AddStringToObject(root, "language", language.c_str());
    const bool written = writeConfig(root);
    cJSON_Delete(root);
    lunar::diagnosticLog("ui-i18n", "language override=%s saved=%s",
                         language.c_str(), written ? "true" : "false");
    return written;
}

std::string getLanguageRestartNotice(const std::string& language) {
    const std::string locale = resolveNoticeLocale(language);
    const std::string path = "romfs:/i18n/" + locale + "/lunarnx.json";
    cJSON* root = readJsonObject(path.c_str());
    if (root) {
        cJSON* settings = cJSON_GetObjectItemCaseSensitive(root, "settings");
        cJSON* notice = settings && cJSON_IsObject(settings)
            ? cJSON_GetObjectItemCaseSensitive(settings, "language_restart")
            : nullptr;
        if (notice && cJSON_IsString(notice) && notice->valuestring) {
            std::string text = notice->valuestring;
            cJSON_Delete(root);
            return text;
        }
        cJSON_Delete(root);
    }

    lunar::diagnosticLog("ui-i18n", "restart notice missing locale=%s",
                         locale.c_str());
    return brls::getStr("lunarnx/settings/language_restart");
}

void configureAppLocale() {
    const std::string language = getConfiguredLanguage();
    brls::Platform::APP_LOCALE_DEFAULT = language;
    lunar::diagnosticLog("ui-i18n", "startup language=%s",
                         language.c_str());
}

} // namespace lunar::ui
