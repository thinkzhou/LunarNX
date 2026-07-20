#include "stream/audio_decoder.h"

#include <cassert>
#include <cstdint>
#include <vector>

using lunar::stream::AudioDecoder;
using lunar::stream::AudioFrame;

int main() {
    AudioDecoder decoder;
    std::vector<AudioFrame> frames;
    decoder.setCallback([&frames](const AudioFrame& frame) {
        frames.push_back(frame);
    });

    assert(decoder.initialize());
    const uint8_t silence[] = {0xf8, 0xff, 0xfe};
    assert(decoder.decode(silence, sizeof(silence), 1'000'000'000ULL));
    assert(frames.size() == 1);
    assert(frames.back().sample_count == 960);
    assert(decoder.lastFrameSamples() == 960);

    assert(decoder.decodeMissing(1'020'000'000ULL));
    assert(frames.size() == 2);
    assert(frames.back().sample_count == 960);
    assert(frames.back().timestamp == 1'020'000'000ULL);
    decoder.shutdown();
    return 0;
}
