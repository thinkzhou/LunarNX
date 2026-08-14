#pragma once

#ifdef __SWITCH__

#include "ps_console.h"
#include "psn_auth_manager.h"
#include "ps_remote_connector.h"
#include "../app/stream_runtime.h"
#include "../stream/media_pipeline.h"
#include "../stream/stream_backend_provider.h"
#include "../stream/perf_stats.h"
#include "../input/gamepad_reader.h"
#include "../input/rumble_controller.h"
#include "ps_input_mapper.h"
#include "ps_touchpad_reader.h"
#include "ps_mock_replay_session.h"
#include "ps_stream_session.h"
#include <chiaki/log.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>

namespace lunar::ps {

class PsStreamController : public app::IStreamRuntime {
public:
    using LaunchCallback = std::function<void(app::StreamState, const std::string&)>;
    using LoginPinCallback = std::function<void(bool incorrect)>;

    PsStreamController(const PsConsole& console,
                       const std::string& psn_access_token,
                       const std::string& psn_account_id,
                       int width, int height, int fps, int bitrate_kbps,
                       stream::VideoCodec video_codec = stream::VideoCodec::H264);
    ~PsStreamController() override;

    // IStreamRuntime
    void stopStream(bool set_disconnected) override;
    app::StreamState getState() const override { return state_.load(); }
    const stream::PerfStats& getPerfStats() const override { return perf_; }
    int getStreamWidth() const override { return width_; }
    int getStreamHeight() const override { return height_; }
    stream::VideoBackend getDefaultVideoBackend() const override { return video_backend_; }
    stream::VideoCodec getVideoCodec() const override { return video_codec_; }
    app::StreamPlatform getStreamPlatform() const override {
        return app::StreamPlatform::PlayStation;
    }
    void setInputSuppressed(bool suppressed) override;
    void requestPlatformHomeButton() override { ps_button_requested_ = true; }
    bool resumeAfterForeground() override;
    app::TouchpadFeedback getTouchpadFeedback() const override;
    void update() override;
    void presentVideoFrame() override;

    // PS-specific
    bool startStream();
    void requestCancel();
    void setRumbleEnabled(bool enabled);
    void setRumbleStrengthPercent(int percent);
    bool setPsnCredentials(std::string access_token, std::string account_id);
    std::string lastError() const { return last_error_; }
    void setLaunchCallback(LaunchCallback cb);
    void setLoginPinCallback(LoginPinCallback cb);
    void submitLoginPin(const std::string& pin);

private:
    void setState(app::StreamState s, const std::string& info = "");
    void startInputLoop();
    void stopInputLoop();
    void startVideoMonitor();
    void stopVideoMonitor();
    bool requestRecoveryIDR();
    void releasePendingRemoteResult();

    PsConsole console_;
    std::string psn_access_token_;
    std::string psn_account_id_;
    int width_, height_, fps_, bitrate_kbps_;
    stream::VideoCodec video_codec_;
    stream::VideoBackend video_backend_;

    // Components
    std::unique_ptr<stream::StreamBackendProvider> stream_backend_;
    std::unique_ptr<stream::MediaPipeline> media_;
    std::unique_ptr<PsMediaBridge> bridge_;
    std::unique_ptr<PsStreamSession> session_;
    std::unique_ptr<PsMockReplaySession> mock_session_;
    std::unique_ptr<input::GamepadReader> gamepad_;
    std::unique_ptr<input::RumbleController> rumble_;
    std::unique_ptr<PsInputMapper> input_mapper_;
    std::unique_ptr<PsTouchpadReader> touchpad_reader_;
    ChiakiLog remote_log_{};
    std::unique_ptr<PsRemoteConnector> remote_connector_;
    PsRemoteResult remote_result_;

    // State
    std::atomic<app::StreamState> state_{app::StreamState::Idle};
    stream::PerfStats perf_;
    std::string last_error_;
    std::string launch_info_;
    LaunchCallback launch_callback_;
    LoginPinCallback login_pin_callback_;
    mutable std::mutex callback_mutex_;
    mutable std::mutex remote_connector_mutex_;
    // Start/stop mutate the session graph exclusively. Input sampling and
    // zero-copy presentation only need the graph to stay alive, and must not
    // serialize with each other: a slow GPU present would otherwise delay
    // controller packets.
    mutable std::shared_mutex stream_operation_mutex_;
    std::atomic<bool> input_suppressed_{false};
    mutable std::mutex touchpad_feedback_mutex_;
    app::TouchpadFeedback touchpad_feedback_{};
    std::atomic<bool> rumble_enabled_{true};
    std::atomic<int> rumble_strength_percent_{50};
    std::atomic<bool> ps_button_requested_{false};
    int ps_button_pulse_frames_remaining_ = 0;
    bool ps_button_release_pending_ = false;
    uint32_t last_input_buttons_ = 0;
    uint32_t input_transition_logs_ = 0;
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> video_monitor_stop_{false};
    std::atomic<bool> stream_transport_connected_{false};
    std::atomic<bool> input_loop_stop_{true};
    std::thread input_thread_;
    std::thread video_monitor_thread_;
    std::mutex video_recovery_mutex_;
    std::chrono::steady_clock::time_point last_recovery_request_{};
    std::chrono::steady_clock::time_point last_stats_sample_{};
};

} // namespace lunar::ps

#endif
