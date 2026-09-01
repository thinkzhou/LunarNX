#pragma once

#include <algorithm>
#include <cstdint>

namespace lunar::stream {

struct Nv12TextureGeometry {
    uint32_t luma_width = 0;
    uint32_t luma_height = 0;
    uint32_t chroma_width = 0;
    uint32_t chroma_height = 0;
    uint32_t storage_luma_width = 0;
    uint32_t storage_luma_height = 0;
    uint32_t storage_chroma_width = 0;
    uint32_t storage_chroma_height = 0;

    constexpr bool valid() const {
        return luma_width != 0 && luma_height != 0 &&
               chroma_width != 0 && chroma_height != 0 &&
               storage_luma_width != 0 && storage_luma_height != 0 &&
               storage_chroma_width != 0 && storage_chroma_height != 0;
    }
};

constexpr uint32_t nv12AlignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

constexpr Nv12TextureGeometry makeNv12TextureGeometry(
    int frame_width,
    int frame_height,
    int luma_pitch,
    int chroma_pitch) {
    if (frame_width <= 0 || frame_height <= 0 ||
        luma_pitch <= 0 || chroma_pitch <= 0) {
        return {};
    }

    const uint32_t width = static_cast<uint32_t>(frame_width);
    const uint32_t height = static_cast<uint32_t>(frame_height);
    const uint32_t chroma_width = (width + 1u) / 2u;
    const uint32_t chroma_height = (height + 1u) / 2u;

    return {
        width,
        height,
        chroma_width,
        chroma_height,
        std::max(static_cast<uint32_t>(luma_pitch), width),
        nv12AlignUp(height, 32u),
        std::max((static_cast<uint32_t>(chroma_pitch) + 1u) / 2u,
                 chroma_width),
        nv12AlignUp(chroma_height, 16u),
    };
}

} // namespace lunar::stream
