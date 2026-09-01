#pragma once
#include "../auth/auth_manager.h"
#include "../api/xbox_api_client.h"
#include "../stream/media_pipeline.h"
#include "../stream/stream_backend_provider.h"
#include "../input/gamepad_reader.h"
#include "../input/xinput_encoder.h"
#include "../input/rumble_controller.h"
#include "../stream/perf_stats.h"
#include "web_rtc_transport.h"
#include "xbox_channel_manager.h"
#include "xbox_session_client.h"
#include "xbox_stream_session.h"
#include "stream_profile.h"
#include "stream_runtime.h"
#include <string>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace lunar::app {

class StreamController : public IStreamRuntime {
public:
    using StateCallback = std::function<void(StreamState, const std::string&)>;

    StreamController();
    ~StreamController() override;

    // Auth
    bool startAuth();
    bool pollAuth();
    auth::DeviceCodePollResult pollAuthStatus();
    int getAuthPollIntervalSeconds() const;
    int getDeviceCodeExpiresInSeconds() const;
    bool hasCredentials() const;
    bool isMockMode() const { return mock_mode_.load(); }
    bool loadTokens(const std::string& path);
    bool saveTokens(const std::string& path);
    void signOut();
    std::string getAuthError() const;
    std::string getGamertag() const;
    std::string getAuthCode() const;
    std::string getAuthUrl() const;

    // Console / cloud titles
    bool fetchConsoles();
    std::vector<api::XboxConsole> getConsoles() const;
    std::string getConsoleFetchError() const;
    bool hasCloudAccess() const;
    bool restoreCloudTitlesFromCache();
    bool loadConsoleCache();
    bool fetchCloudTitles(bool force_refresh = false);
    std::vector<api::CloudTitle> getCloudTitles() const;          // full library
    std::vector<api::CloudTitle> getRecentCloudTitles() const;    // recent subset
    std::vector<api::CloudTitle> getNewCloudTitles() const;       // newly added
    std::string getCloudTitleFetchError() const;

    // Connect & Stream
    bool startStream(const std::string& server_id, int width = 1280, int height = 720);
    bool startStream(const std::string& server_id, int width, int height,
                     const stream::MediaPipelineOptions& options);
    bool startStream(const std::string& server_id, int width, int height,
                     const stream::MediaPipelineOptions& options,
                     int bitrate_kbps);
    bool startCloudStream(const std::string& title_id, int width = 1280, int height = 720);
    bool startCloudStream(const std::string& title_id, int width, int height,
                          const stream::MediaPipelineOptions& options);
    bool startCloudStream(const std::string& title_id, int width, int height,
                          const stream::MediaPipelineOptions& options,
                          int bitrate_kbps);
    void stopStream();
    void requestStop() override;
    void stopStream(bool set_disconnected) override;
    StreamState getState() const override { return state_.load(); }
    std::string getLastStreamError() const;

    const stream::PerfStats& getPerfStats() const override { return perf_; }
    int getStreamWidth() const override { return stream_width_; }
    int getStreamHeight() const override { return stream_height_; }
    void setStateCallback(StateCallback cb);
    input::StreamInputRouter& inputRouter() override { return input_router_; }
    void requestPlatformHomeButton() override;
    bool resumeAfterForeground(CancelCallback cancel = {}) override;
    void requestGuideButton();
    bool consumeGuideButtonRequest();
    void setBaseUrl(const std::string& url) { base_url_ = url; }
    void setForceRegionIp(const std::string& ip);
    std::string getForceRegionIp() const;
    void setPreferredGameLanguage(const std::string& locale);
    std::string getPreferredGameLanguage() const;
    void setDefaultVideoBackend(stream::VideoBackend backend);
    stream::VideoBackend getDefaultVideoBackend() const override;
    stream::VideoCodec getVideoCodec() const override {
        return stream::VideoCodec::H264;
    }
    StreamPlatform getStreamPlatform() const override { return StreamPlatform::Xbox; }
    void setRumbleEnabled(bool enabled);
    bool getRumbleEnabled() const { return rumble_enabled_.load(); }
    void setRumbleStrengthPercent(int percent);
    int getRumbleStrengthPercent() const {
        return rumble_strength_percent_.load();
    }

    // Bypass real auth for local/testing. Creates a mock API client with
    // dummy tokens that the mock_xbox server will accept.
    bool bypassAuthForMock(const std::string& base_url);

    // Per-frame update for input + rendering
    void update() override;
    void presentVideoFrame() override;
    void setVideoPresentationSuspended(bool suspended) override;

private:
    api::HttpClient& http();
    auth::AuthManager& auth();
    void ensureStreamingComponents();
    void setState(StreamState s, const std::string& info = "");
    bool isStreamCancelled(uint32_t generation) const;
    void requestStreamStop();
    void cleanupStreamResources(bool set_disconnected);
    bool startStreamWithProfile(const StreamProfile& profile,
                                const stream::MediaPipelineOptions& options,
                                CancelCallback cancel = {});
    std::shared_ptr<api::XboxApiClient> makeApiClient(SessionType type);

    std::unique_ptr<api::HttpClient> http_;
    std::unique_ptr<auth::AuthManager> auth_;
    std::shared_ptr<api::XboxApiClient> api_;
    std::unique_ptr<WebRtcTransport> transport_;
    std::unique_ptr<XboxChannelManager> channels_;
    std::unique_ptr<XboxSessionClient> session_client_;
    std::unique_ptr<XboxStreamSession> stream_session_;
    std::unique_ptr<stream::StreamBackendProvider> stream_backend_;
    std::unique_ptr<stream::MediaPipeline> media_;
    std::unique_ptr<input::GamepadReader> gamepad_;
    std::unique_ptr<input::XInputEncoder> xinput_;
    std::unique_ptr<input::RumbleController> rumble_;

    std::atomic<StreamState> state_{StreamState::Idle};
    StateCallback state_cb_;
    mutable std::mutex state_callback_mutex_;
    std::vector<api::XboxConsole> consoles_;
    std::vector<api::CloudTitle> cloud_titles_;
    std::vector<api::CloudTitle> recent_cloud_titles_;
    std::vector<api::CloudTitle> new_cloud_titles_;
    bool loadCloudLibraryCache(bool allow_stale = false);
    void saveCloudLibraryCache() const;
    void saveConsoleCache() const;
    std::string last_console_error_;
    std::string last_cloud_title_error_;
    std::string last_stream_error_;
    SessionType active_session_type_ = SessionType::Home;
    std::string session_id_;
    mutable std::mutex stream_lifecycle_mutex_;
    std::mutex stream_operation_mutex_;
    std::mutex auth_operation_mutex_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<uint32_t> stream_generation_{0};
    input::StreamInputRouter input_router_;
    std::atomic<bool> guide_button_requested_{false};
    std::atomic<bool> signing_out_{false};
    std::atomic<bool> mock_mode_{false};
    std::atomic<stream::VideoBackend> default_video_backend_{stream::VideoBackend::HardwareZeroCopy};
    std::atomic<bool> rumble_enabled_{true};
    std::atomic<int> rumble_strength_percent_{50};
    stream::PerfStats perf_;
    int stream_width_ = 1280;
    int stream_height_ = 720;
    std::string base_url_;
    std::string preferred_game_language_ = "en-US";
    StreamProfile active_profile_;
    stream::MediaPipelineOptions active_media_options_;
    bool has_active_profile_ = false;
};

} // namespace lunar::app
