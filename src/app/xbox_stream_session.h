#pragma once

#include "../input/gamepad_reader.h"
#include "../input/rumble_controller.h"
#include "../input/stream_input_router.h"
#include "../input/xinput_encoder.h"
#include "../stream/media_pipeline.h"
#include "../stream/perf_stats.h"
#include "stream_profile.h"
#include "web_rtc_transport.h"
#include "xbox_channel_manager.h"
#include "xbox_session_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace lunar::app {

class XboxStreamSession {
public:
    struct RuntimeCallbacks {
        std::function<void(const std::string&)> on_status;
        std::function<void()> on_streaming;
        std::function<void(const std::string&)> on_cancelled;
        std::function<void(const std::string&)> on_error;
        std::function<void(const std::string&)> on_session_id;
        std::function<bool()> external_cancel;
        std::function<bool()> consume_guide_button;
        std::function<bool()> refresh_tokens;
    };

    XboxStreamSession(XboxSessionClient& session_client,
                      WebRtcTransport& transport,
                      XboxChannelManager& channels,
                      stream::MediaPipeline& media,
                      input::GamepadReader& gamepad,
                      input::XInputEncoder& xinput,
                      input::RumbleController& rumble,
                      input::StreamInputRouter& input_router,
                      stream::PerfStats& perf);
    ~XboxStreamSession();

    bool start(const StreamProfile& profile,
               const stream::MediaPipelineOptions& media_options,
               RuntimeCallbacks callbacks);
    void stop(bool delete_session = true);

    bool isStreaming() const { return streaming_.load(); }
    std::string sessionId() const;

private:
    bool isCancelled(const RuntimeCallbacks& callbacks) const;
    bool sleepUnlessCancelled(std::chrono::milliseconds duration,
                              const RuntimeCallbacks& callbacks) const;
    bool sleepUntilCancelled(std::chrono::steady_clock::time_point deadline,
                             const RuntimeCallbacks& callbacks) const;
    bool negotiateWebRtc(const StreamProfile& profile,
                         const std::string& session_id,
                         const RuntimeCallbacks& callbacks);
    webrtc::PeerCallbacks createPeerCallbacks();
    void runLoop(StreamProfile profile,
                 std::string session_id,
                 int keep_alive_seconds,
                 RuntimeCallbacks callbacks);
    void controlLoop(std::string session_id,
                     int keep_alive_seconds,
                     RuntimeCallbacks callbacks);
    void cleanupResources(bool delete_session);
    bool failStart(const std::string& reason, const RuntimeCallbacks& callbacks);
    bool cancelStart(const RuntimeCallbacks& callbacks);

    XboxSessionClient& session_client_;
    WebRtcTransport& transport_;
    XboxChannelManager& channels_;
    stream::MediaPipeline& media_;
    input::GamepadReader& gamepad_;
    input::XInputEncoder& xinput_;
    input::RumbleController& rumble_;
    input::StreamInputRouter& input_router_;
    stream::PerfStats& perf_;

    std::atomic<bool> streaming_{false};
    std::atomic<bool> stop_requested_{false};
    mutable std::mutex state_mutex_;
    std::mutex session_api_mutex_;
    std::mutex control_mutex_;
    std::condition_variable control_cv_;
    std::thread stream_thread_;
    std::thread control_thread_;
    std::string session_id_;
};

} // namespace lunar::app
