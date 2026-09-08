#pragma once

#ifdef __SWITCH__

#include "steam_link_client.h"
#include "stream_support.h"
#include "steam_hid.h"
#include "steam_sensors.h"
#include "steam_cursor.h"
#include "../input/rumble_controller.h"
#include "../app/stream_runtime.h"
#include "../input/gamepad_reader.h"
#include "../stream/media_pipeline.h"
#include "../stream/perf_stats.h"
#include "../stream/stream_backend_provider.h"

extern "C" {
#include <ihslib/session.h>
}

#include <atomic>
#include <array>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace lunar::steamlink {

class SteamLinkStreamController final : public app::IStreamRuntime {
public:
    SteamLinkStreamController(std::shared_ptr<SteamLinkClient> client,
                              SteamLinkHost host, std::string security_pin,
                              int width, int height);
    ~SteamLinkStreamController() override;

    SteamLinkStreamController(const SteamLinkStreamController&) = delete;
    SteamLinkStreamController& operator=(const SteamLinkStreamController&) = delete;

    bool startStream();
    void configurePointer(TouchMode touch, GyroMode gyro);
    std::pair<TouchMode, GyroMode> pointerModes() const;
    void reloadInputMapping();
    std::string lastError() const;
    CursorSnapshot cursorSnapshot() const { return cursor_.snapshot(); }

    void requestStop() override;
    void stopStream(bool set_disconnected) override;
    app::StreamState getState() const override { return state_.load(); }
    const stream::PerfStats& getPerfStats() const override { return perf_; }
    int getStreamWidth() const override { return width_; }
    int getStreamHeight() const override { return height_; }
    stream::VideoBackend getDefaultVideoBackend() const override { return video_backend_; }
    stream::VideoCodec getVideoCodec() const override { return stream::VideoCodec::H264; }
    app::StreamPlatform getStreamPlatform() const override {
        return app::StreamPlatform::Steam;
    }
    input::StreamInputRouter& inputRouter() override { return input_router_; }
    void requestPlatformHomeButton() override { guide_requested_ = true; }
    bool resumeAfterForeground(CancelCallback cancel = {}) override;
    void update() override;
    void presentVideoFrame() override;
    void setVideoPresentationSuspended(bool suspended) override;

private:
    struct StreamWaitState {
        std::mutex mutex;
        std::condition_variable condition;
        bool done = false;
        bool success = false;
        SteamLinkStreamInfo info{};
        std::string error;
    };

    static void onSessionInitialized(IHS_Session*, void* context);
    static void onSessionConnecting(IHS_Session*, void* context);
    static void onSessionConfiguring(IHS_Session*, IHS_SessionConfig* config, void* context);
    static void onSessionConnected(IHS_Session*, void* context);
    static void onSessionDisconnected(IHS_Session*, void* context);
    static void onSessionFinalized(IHS_Session*, void* context);

    static int onAudioStart(IHS_Session*, const IHS_StreamAudioConfig*, void* context);
    static int onAudioSubmit(IHS_Session*, IHS_Buffer*, void* context);
    static void onAudioStop(IHS_Session*, void* context);
    static int onVideoStart(IHS_Session*, const IHS_StreamVideoConfig*, void* context);
    static IHS_StreamVideoSubmitResult onVideoSubmit(IHS_Session*, IHS_Buffer*,
                                                      IHS_StreamVideoFrameFlag, void* context);
    static void onVideoStop(IHS_Session*, void* context);
    static int onVideoCaptureSize(IHS_Session*, int, int, void* context);
    static void onVideoFramerate(IHS_Session*, uint32_t, uint32_t, uint32_t, void* context);
    static void onVideoBitrate(IHS_Session*, int32_t, void* context);
    static void onVideoQuality(IHS_Session*, int32_t, void* context);
    static void onVideoBitrateOverride(IHS_Session*, int32_t, void* context);

    static bool onSetCursor(IHS_Session*, uint64_t, void* context);
    static bool onDeleteCursor(IHS_Session*, uint64_t, void* context);
    static void onCursorImage(IHS_Session*, const IHS_StreamInputCursorImage*, void* context);
    static void onShowCursor(IHS_Session*, float, float, void* context);
    static void onHideCursor(IHS_Session*, void* context);
    static void onCapsLock(IHS_Session*, bool, void* context);
    static void onKeymap(IHS_Session*, const IHS_KeymapEntry*, size_t, void* context);

    bool initializeMedia();
    bool initializeSession(const SteamLinkStreamInfo& info);
    void setState(app::StreamState state, const std::string& detail = {});
    void setLastError(std::string error);
    uint64_t mediaTimestampNs();

    std::shared_ptr<SteamLinkClient> client_;
    SteamLinkHost host_;
    std::string security_pin_;
    int width_ = 1280;
    int height_ = 720;
    stream::VideoBackend video_backend_ = stream::VideoBackend::HardwareZeroCopy;

    std::unique_ptr<stream::StreamBackendProvider> stream_backend_;
    std::unique_ptr<stream::MediaPipeline> media_;
    std::unique_ptr<input::GamepadReader> gamepad_;
    IHS_Session* session_ = nullptr;
    IHS_HIDProvider* hid_provider_ = nullptr;
    std::shared_ptr<SteamPadState> pad_state_;
    std::unique_ptr<input::RumbleController> rumble_;
    uint64_t rumble_generation_ = 0;
    uint64_t guide_until_ns_ = 0;
    bool hid_announced_ = false;
    SteamPointer pointer_;
    SteamCursor cursor_;
    std::unique_ptr<SteamSensors> sensors_;
    bool mouse_left_ = false, mouse_right_ = false;

    std::atomic<app::StreamState> state_{app::StreamState::Idle};
    StreamCancellation cancellation_;
    InputPump input_pump_;
    StartupWatchdog startup_watchdog_;
    MediaActivityWatchdog media_activity_;
    VideoProgressWatchdog video_progress_;
    bool presentation_suspended_ = false;
    uint64_t status_log_at_ns_ = 0;
    bool logged_hid_open_ = false;
    InputAvailabilityWatchdog input_availability_;
    VideoRecoveryFeedback video_recovery_;
    SessionDisconnectGate disconnect_gate_;
    LogThrottle hid_announce_log_;
    LogThrottle analog_log_;
    std::vector<uint8_t> video_parameters_;
    std::atomic<bool> session_connected_{false};
    std::atomic<bool> guide_requested_{false};
    std::atomic<uint64_t> media_epoch_ns_{0};
    std::atomic<uint32_t> video_samples_{0};
    std::atomic<uint32_t> audio_samples_{0};
    std::atomic<uint16_t> audio_sequence_{0};
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex error_mutex_;
    std::string last_error_;
    input::StreamInputRouter input_router_;
    stream::PerfStats perf_;

};

} // namespace lunar::steamlink

#endif
