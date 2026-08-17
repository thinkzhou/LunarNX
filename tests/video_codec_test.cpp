#include "stream/video_codec.h"

#include <cstdio>
#include <initializer_list>
#include <vector>

using lunar::stream::VideoCodec;
using lunar::stream::inspectVideoAccessUnit;
using lunar::stream::inspectXboxH264AccessUnit;

namespace {

std::vector<uint8_t> annexB(std::initializer_list<uint8_t> headers) {
    std::vector<uint8_t> data;
    for (uint8_t header : headers) {
        data.insert(data.end(), {0, 0, 0, 1, header, 0});
    }
    return data;
}

bool require(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

} // namespace

int main() {
    const auto h264 = annexB({0x67, 0x68, 0x65}); // SPS, PPS, IDR
    const auto h264_info = inspectVideoAccessUnit(
        VideoCodec::H264, h264.data(), h264.size());
    if (!require(h264_info.hasRequiredParameterSets(VideoCodec::H264),
                 "H264 SPS/PPS should be recognized") ||
        !require(h264_info.has_random_access && h264_info.has_vcl,
                 "H264 IDR should be random access VCL")) return 1;

    const auto xbox_info = inspectXboxH264AccessUnit(h264.data(), h264.size());
    if (!require(xbox_info.has_sps && xbox_info.has_pps &&
                     xbox_info.has_random_access && xbox_info.has_vcl,
                 "Xbox fast path should recognize H264 startup data") ||
        !require(xbox_info.nal_types.empty(),
                 "Xbox fast path must not construct NAL strings")) return 1;

    const auto hevc = annexB({0x40, 0x42, 0x44, 0x26}); // VPS, SPS, PPS, IDR_W_RADL
    const auto hevc_info = inspectVideoAccessUnit(
        VideoCodec::HEVC, hevc.data(), hevc.size());
    if (!require(hevc_info.hasRequiredParameterSets(VideoCodec::HEVC),
                 "HEVC VPS/SPS/PPS should be recognized") ||
        !require(hevc_info.has_random_access && hevc_info.has_vcl,
                 "HEVC IDR should be random access VCL")) return 1;

    const auto cra = annexB({0x2a}); // CRA_NUT type 21
    const auto cra_info = inspectVideoAccessUnit(
        VideoCodec::HEVC, cra.data(), cra.size());
    if (!require(cra_info.has_random_access,
                 "HEVC CRA should allow decoder recovery")) return 1;

    const auto p_slice = annexB({0x02}); // TRAIL_R type 1
    const auto p_info = inspectVideoAccessUnit(
        VideoCodec::HEVC, p_slice.data(), p_slice.size());
    if (!require(p_info.has_vcl && !p_info.has_random_access,
                 "HEVC inter frame must not open recovery gate")) return 1;

    std::puts("Video codec access-unit tests passed");
    return 0;
}
