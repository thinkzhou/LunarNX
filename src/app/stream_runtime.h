#pragma once

#include <array>
#include <cstdint>

namespace lunar::stream {

struct PerfStats;
enum class VideoBackend;
enum class VideoCodec;

} // namespace lunar::stream

namespace lunar::app {

enum class StreamState {
    Idle,
    Authenticating,
    Connecting,
    Streaming,
    Disconnected,
    Error,
};

enum class StreamPlatform {
    Xbox,
    PlayStation,
};

enum class TouchpadFeedbackGesture : uint8_t {
    None,
    Touch,
    Tap,
    Pan,
    LongPress,
};

struct TouchpadFeedbackPoint {
    bool active = false;
    uint16_t screen_x = 0;
    uint16_t screen_y = 0;
};

struct TouchpadFeedback {
    TouchpadFeedbackGesture gesture = TouchpadFeedbackGesture::None;
    std::array<TouchpadFeedbackPoint, 2> points{};
};

// Protocol-neutral runtime surface consumed by the in-stream UI. Protocol
// discovery, registration and session setup intentionally remain outside this
// boundary.
class IStreamRuntime {
public:
    virtual ~IStreamRuntime() = default;

    virtual void stopStream(bool set_disconnected) = 0;
    virtual StreamState getState() const = 0;

    virtual const stream::PerfStats& getPerfStats() const = 0;
    virtual int getStreamWidth() const = 0;
    virtual int getStreamHeight() const = 0;
    virtual stream::VideoBackend getDefaultVideoBackend() const = 0;
    virtual stream::VideoCodec getVideoCodec() const = 0;
    virtual StreamPlatform getStreamPlatform() const = 0;

    virtual void setInputSuppressed(bool suppressed) = 0;
    virtual void requestPlatformHomeButton() = 0;
    // Called from a network worker after Switch returns to the foreground.
    // Implementations keep a healthy session alive and rebuild a dead one.
    virtual bool resumeAfterForeground() = 0;
    virtual TouchpadFeedback getTouchpadFeedback() const { return {}; }
    virtual void update() = 0;
    virtual void presentVideoFrame() = 0;
};

} // namespace lunar::app
