#include "stream_backend_provider.h"
#include "audio_decoder.h"
#include "audio_player.h"
#include "av_sync.h"
#include "video_decoder.h"
#include "video_renderer.h"

namespace lunar::stream {
namespace {

class DefaultStreamBackendProvider final : public StreamBackendProvider {
public:
    std::unique_ptr<VideoDecoder> createVideoDecoder() override {
        return std::make_unique<VideoDecoder>();
    }

    std::unique_ptr<VideoRenderer> createVideoRenderer() override {
        return std::make_unique<VideoRenderer>();
    }

    std::unique_ptr<AudioDecoder> createAudioDecoder() override {
        return std::make_unique<AudioDecoder>();
    }

    std::unique_ptr<AudioPlayer> createAudioPlayer() override {
        return std::make_unique<AudioPlayer>();
    }

    std::unique_ptr<AVSync> createAVSync() override {
        return std::make_unique<AVSync>();
    }
};

} // namespace

std::unique_ptr<StreamBackendProvider> StreamBackendProvider::createDefault() {
    return std::make_unique<DefaultStreamBackendProvider>();
}

} // namespace lunar::stream
