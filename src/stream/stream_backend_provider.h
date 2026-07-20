#pragma once

#include <memory>

namespace lunar::stream {

class AudioDecoder;
class AudioPlayer;
class AVSync;
class VideoDecoder;
class VideoRenderer;

/// Factory boundary for the platform-specific streaming media stack.
///
/// This mirrors Moonlight-Switch's DecoderAndRenderProvider shape without
/// forcing the Xbox/WebRTC session code to follow Moonlight's protocol layer.
class StreamBackendProvider {
public:
    virtual ~StreamBackendProvider() = default;

    virtual std::unique_ptr<VideoDecoder> createVideoDecoder() = 0;
    virtual std::unique_ptr<VideoRenderer> createVideoRenderer() = 0;
    virtual std::unique_ptr<AudioDecoder> createAudioDecoder() = 0;
    virtual std::unique_ptr<AudioPlayer> createAudioPlayer() = 0;
    virtual std::unique_ptr<AVSync> createAVSync() = 0;

    static std::unique_ptr<StreamBackendProvider> createDefault();
};

} // namespace lunar::stream
