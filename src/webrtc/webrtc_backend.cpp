#include "webrtc_backend.h"
#include "../common.h"
#include <cJSON.h>
#include <cstdio>
#include <string>
namespace lunar::webrtc {
WebRtcBackend loadWebRtcBackend() {
    FILE* file = std::fopen(lunar::get_config_path(), "rb");
    if (!file) return WebRtcBackend::Libpeer;
    std::string data;
    char chunk[1024]; size_t count;
    while ((count = std::fread(chunk, 1, sizeof(chunk), file)) != 0 && data.size() < 65536) data.append(chunk, count);
    std::fclose(file);
    if (data.size() > 65536) return WebRtcBackend::Libpeer;
    cJSON* config = cJSON_Parse(data.c_str());
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(config, "xbox_webrtc_backend");
    const auto backend = parseWebRtcBackend(cJSON_IsString(value) ? value->valuestring : nullptr);
    cJSON_Delete(config);
    return backend;
}
}
