#include "stream/nv12_texture_geometry.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>

using lunar::stream::Nv12TextureGeometry;
using lunar::stream::makeNv12TextureGeometry;

namespace {

float lastSampleTexel(uint32_t source_height, uint32_t output_height) {
    const float fragment_v =
        (static_cast<float>(output_height) - 0.5f) /
        static_cast<float>(output_height);
    return fragment_v * static_cast<float>(source_height) - 0.5f;
}

void test_720p_keeps_padding_outside_logical_texture() {
    const Nv12TextureGeometry geometry =
        makeNv12TextureGeometry(1280, 720, 1280, 1280);

    assert(geometry.luma_width == 1280);
    assert(geometry.luma_height == 720);
    assert(geometry.chroma_width == 640);
    assert(geometry.chroma_height == 360);
    assert(geometry.storage_luma_height == 736);
    assert(geometry.storage_chroma_height == 368);

    const float last_chroma_sample =
        lastSampleTexel(geometry.chroma_height, 720);
    const uint32_t upper_linear_tap =
        static_cast<uint32_t>(std::floor(last_chroma_sample)) + 1u;

    // The upper tap is the first padding row in storage, but it is outside the
    // visible descriptor and therefore clamps back to the final visible row.
    assert(upper_linear_tap == geometry.chroma_height);
    assert(upper_linear_tap < geometry.storage_chroma_height);
    assert(std::min(upper_linear_tap, geometry.chroma_height - 1u) == 359u);
}

void test_1080p_keeps_coded_alignment_outside_logical_texture() {
    const Nv12TextureGeometry geometry =
        makeNv12TextureGeometry(1920, 1080, 1920, 1920);

    assert(geometry.luma_width == 1920);
    assert(geometry.luma_height == 1080);
    assert(geometry.chroma_width == 960);
    assert(geometry.chroma_height == 540);
    assert(geometry.storage_luma_height == 1088);
    assert(geometry.storage_chroma_height == 544);
}

void test_odd_dimensions_round_chroma_up() {
    const Nv12TextureGeometry geometry =
        makeNv12TextureGeometry(1279, 719, 1280, 1280);

    assert(geometry.luma_width == 1279);
    assert(geometry.luma_height == 719);
    assert(geometry.chroma_width == 640);
    assert(geometry.chroma_height == 360);
}

} // namespace

int main() {
    test_720p_keeps_padding_outside_logical_texture();
    test_1080p_keeps_coded_alignment_outside_logical_texture();
    test_odd_dimensions_round_chroma_up();
    std::cout << "NV12 texture geometry tests passed\n";
    return 0;
}
