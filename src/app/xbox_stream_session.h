#pragma once

#include "../input/gamepad_reader.h"
#include "../input/rumble_controller.h"
#include "../input/stream_input_router.h"
#include "../input/xinput_encoder.h"
#include "../input/xbox_input_accumulator.h"
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
#include <vector>

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
        // force=true is used only after a keep-alive 401/403. It bypasses
        // the normal expiry throttle and obtains a fresh streaming token.
        std::function<bool(bool force)> refresh_tokens;
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
    bool reconnectWithFreshSession(const StreamProfile& profile,
                                   std::string& session_id,
                                   int& keep_alive_seconds,
                                   const RuntimeCallbacks& callbacks,
                                   bool& reconnect_prepared,
                                   const std::string& reconnect_reason);
    void prepareFreshSessionReconnect(const char* reason);
    webrtc::PeerCallbacks createPeerCallbacks();
    void runLoop(StreamProfile profile,
                 std::string session_id,
                 int keep_alive_seconds,
                 RuntimeCallbacks callbacks);
    void startInputLoop(RuntimeCallbacks callbacks);
    void stopInputLoop();
    void sampleInput(const RuntimeCallbacks& callbacks,
                     int& guide_pulse_frames_remaining,
                     bool& guide_release_pending);
    void prepareInputForReconnect();
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
    std::atomic<bool> input_loop_stop_{true};
    std::atomic<bool> input_delivery_ready_{false};
    std::atomic<bool> media_startup_ready_{false};
    std::atomic<bool> control_recovery_requested_{false};
    mutable std::mutex state_mutex_;
    std::mutex session_api_mutex_;
    std::mutex control_mutex_;
    std::condition_variable control_cv_;
    std::thread stream_thread_;
    std::thread control_thread_;
    std::thread input_thread_;
    input::XboxInputAccumulator input_accumulator_;
    std::string session_id_;
};

} // namespace lunar::app
