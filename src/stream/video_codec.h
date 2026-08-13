#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace lunar::stream {

enum class VideoCodec {
    H264,
    HEVC,
};

inline const char* videoCodecName(VideoCodec codec) {
    return codec == VideoCodec::HEVC ? "hevc" : "h264";
}

inline const char* videoCodecOverlayName(VideoCodec codec) {
    return codec == VideoCodec::HEVC ? "HEVC" : "H.264";
}

struct VideoAccessUnitInfo {
    bool has_vps = false;
    bool has_sps = false;
    bool has_pps = false;
    bool has_random_access = false;
    bool has_vcl = false;
    int nal_count = 0;
    std::string nal_types;

    bool hasRequiredParameterSets(VideoCodec codec) const {
        return has_sps && has_pps && (codec != VideoCodec::HEVC || has_vps);
    }

    bool hasParameterSets() const { return has_vps || has_sps || has_pps; }
};

inline void appendVideoNalType(std::string& out, uint8_t type) {
    if (out.size() > 96) return;
    if (!out.empty()) out.push_back(',');
    char text[8] = {};
    std::snprintf(text, sizeof(text), "%u", type);
    out += text;
}

inline void inspectVideoNal(VideoCodec codec, uint8_t header,
                            VideoAccessUnitInfo& info) {
    const uint8_t type = codec == VideoCodec::HEVC
        ? static_cast<uint8_t>((header >> 1) & 0x3f)
        : static_cast<uint8_t>(header & 0x1f);
    info.nal_count++;
    if (codec == VideoCodec::HEVC) {
        info.has_vps = info.has_vps || type == 32;
        info.has_sps = info.has_sps || type == 33;
        info.has_pps = info.has_pps || type == 34;
        info.has_random_access = info.has_random_access ||
            (type >= 16 && type <= 21);
        info.has_vcl = info.has_vcl || type <= 31;
    } else {
        info.has_sps = info.has_sps || type == 7;
        info.has_pps = info.has_pps || type == 8;
        info.has_random_access = info.has_random_access || type == 5;
        info.has_vcl = info.has_vcl || (type >= 1 && type <= 5);
    }
    appendVideoNalType(info.nal_types, type);
}

inline bool findVideoStartCode(const uint8_t* data, size_t len, size_t from,
                               size_t& pos, size_t& code_size) {
    for (size_t i = from; i + 3 <= len; ++i) {
        if (data[i] != 0 || data[i + 1] != 0) continue;
        if (data[i + 2] == 1) {
            pos = i;
            code_size = 3;
            return true;
        }
        if (i + 4 <= len && data[i + 2] == 0 && data[i + 3] == 1) {
            pos = i;
            code_size = 4;
            return true;
        }
    }
    return false;
}

inline VideoAccessUnitInfo inspectVideoAccessUnit(VideoCodec codec,
                                                  const uint8_t* data,
                                                  size_t len) {
    VideoAccessUnitInfo info;
    if (!data || len == 0) return info;

    size_t pos = 0;
    size_t code_size = 0;
    if (!findVideoStartCode(data, len, 0, pos, code_size)) {
        inspectVideoNal(codec, data[0], info);
        return info;
    }

    while (pos < len) {
        const size_t nalu_start = pos + code_size;
        size_t next = len;
        size_t next_code_size = 0;
        findVideoStartCode(data, len, nalu_start, next, next_code_size);
        if (nalu_start < next && nalu_start < len) {
            inspectVideoNal(codec, data[nalu_start], info);
        }
        if (next >= len) break;
        pos = next;
        code_size = next_code_size;
    }
    return info;
}

} // namespace lunar::stream
