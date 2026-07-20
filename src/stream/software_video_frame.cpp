#include "software_video_frame.h"

#include <algorithm>

namespace lunar::stream {

SoftwareVideoFrameSink& SoftwareVideoFrameSink::instance() {
    static SoftwareVideoFrameSink sink;
    return sink;
}

void SoftwareVideoFrameSink::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    rgba_.clear();
    width_ = 0;
    height_ = 0;
    timestamp_ = 0;
    ++generation_;
}

bool SoftwareVideoFrameSink::publishRgba(const uint8_t* rgba,
                                         int width,
                                         int height,
                                         uint64_t timestamp) {
    if (!rgba || width <= 0 || height <= 0) return false;

    const size_t size = static_cast<size_t>(width) *
                        static_cast<size_t>(height) * 4;
    std::lock_guard<std::mutex> lock(mutex_);
    rgba_.resize(size);
    std::copy(rgba, rgba + size, rgba_.begin());
    width_ = width;
    height_ = height;
    timestamp_ = timestamp;
    ++generation_;
    return true;
}

SoftwareVideoFrameSnapshot SoftwareVideoFrameSink::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SoftwareVideoFrameSnapshot out;
    out.rgba = rgba_;
    out.width = width_;
    out.height = height_;
    out.timestamp = timestamp_;
    out.generation = generation_;
    return out;
}

} // namespace lunar::stream
