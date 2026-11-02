#include "webrtc_backend.h"
#include "../common.h"
#include "../diagnostics.h"
#ifndef LUNARNX_ENABLE_LIBDATACHANNEL
#define LUNARNX_ENABLE_LIBDATACHANNEL 0
#endif
#include <cJSON.h>
#include <cstdio>
#include <string>
namespace lunar::webrtc {
WebRtcBackend loadWebRtcBackend() {
    FILE* file = std::fopen(lunar::get_config_path(), "rb");
    if (!file) return WebRtcBackend::Libpeer;
    std::string data;
    char chunk[1024]; size_t count;
    bool oversized = false;
    while ((count = std::fread(chunk, 1, sizeof(chunk), file)) != 0) {
        if (count > 65536 - data.size()) { oversized = true; break; }
        data.append(chunk, count);
    }
    const bool read_failed = std::ferror(file) != 0;
    std::fclose(file);
    if (oversized || read_failed) return WebRtcBackend::Libpeer;
    const char* end = nullptr;
    cJSON* config = cJSON_ParseWithLengthOpts(data.c_str(), data.size() + 1, &end, 1);
    if (!config || end != data.c_str() + data.size()) {
        cJSON_Delete(config);
        return WebRtcBackend::Libpeer;
    }
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(config, "xbox_webrtc_backend");
    const auto backend = parseWebRtcBackend(cJSON_IsString(value) ? value->valuestring : nullptr);
    cJSON_Delete(config);
#if LUNARNX_ENABLE_LIBDATACHANNEL
    return backend;
#else
    if (backend == WebRtcBackend::Libdatachannel) {
        lunar::persistentEventLog("webrtc", "requested backend=libdatachannel unavailable; selected backend=libpeer");
    }
    return WebRtcBackend::Libpeer;
#endif
}
}
