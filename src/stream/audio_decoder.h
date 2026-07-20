#pragma once

#include "../common.h"
#include <functional>
#include <vector>
#include <cstdint>

namespace lunar::stream {

struct AudioFrame {
    std::vector<uint8_t> pcm_data;
    size_t sample_count = 0;
    int sample_rate = 48000;
    int channels = 2;
    uint64_t timestamp = 0;
};

class AudioDecoder {
public:
    using FrameCallback = std::function<void(const AudioFrame& frame)>;

    AudioDecoder();
    ~AudioDecoder();

    bool initialize();
    bool decode(const uint8_t* data, size_t len, uint64_t timestamp);
    bool decodeMissing(uint64_t timestamp);
    size_t lastFrameSamples() const { return last_frame_samples_; }
    void setCallback(FrameCallback cb);
    void setPerfStats(struct PerfStats* stats) { perf_ = stats; }
    void shutdown();

private:
    bool decodeInternal(const uint8_t* data,
                        size_t len,
                        int frame_size,
                        uint64_t timestamp,
                        bool plc);

    void* decoder_ = nullptr;
    FrameCallback on_frame_;
    struct PerfStats* perf_ = nullptr;
    bool initialized_ = false;
    size_t last_frame_samples_ = 960;
};

} // namespace lunar::stream
