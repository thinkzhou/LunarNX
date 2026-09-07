#ifdef __SWITCH__

#include "steam_link_stream_controller.h"

#include "../diagnostics.h"
#include "../stream/video_codec.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace lunar::steamlink {
namespace {

constexpr auto kStreamingWait = std::chrono::seconds(20);

uint64_t steadyNowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* stateName(app::StreamState state) {
    switch (state) {
        case app::StreamState::Idle: return "idle";
        case app::StreamState::Authenticating: return "authenticating";
        case app::StreamState::Connecting: return "connecting";
        case app::StreamState::Streaming: return "streaming";
        case app::StreamState::Disconnected: return "disconnected";
        case app::StreamState::Error: return "error";
    }
    return "unknown";
}

} // namespace

SteamLinkStreamController::SteamLinkStreamController(
    std::shared_ptr<SteamLinkClient> client, SteamLinkHost host,
    std::string security_pin, int width, int height)
    : client_(std::move(client)), host_(std::move(host)),
      security_pin_(std::move(security_pin)), width_(width > 0 ? width : 1280),
      height_(height > 0 ? height : 720) {}

SteamLinkStreamController::~SteamLinkStreamController() {
    requestStop();
    stopStream(false);
}

void SteamLinkStreamController::configurePointer(TouchMode touch, GyroMode gyro) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (cancellation_.requested()) return;
    pointer_ = SteamPointer{};
    pointer_.touch_mode = touch;
    pointer_.gyro_mode = gyro;
    pointer_.fenceTouches();
    if (sensors_) {
        sensors_.reset();
        sensors_ = std::make_unique<SteamSensors>(gyro != GyroMode::Off);
    }
    lunar::diagnosticLog("steam-pointer", "settings applied touch=%d gyro=%d", int(touch), int(gyro));
}
std::pair<TouchMode, GyroMode> SteamLinkStreamController::pointerModes() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return {pointer_.touch_mode, pointer_.gyro_mode};
}
void SteamLinkStreamController::reloadInputMapping() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (gamepad_) gamepad_->reloadButtonMapping();
}

std::string SteamLinkStreamController::lastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void SteamLinkStreamController::setLastError(std::string error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = std::move(error);
}

void SteamLinkStreamController::setState(app::StreamState state, const std::string& detail) {
    state_.store(state);
    lunar::diagnosticLog("steam-stream", "state=%s detail=%s",
                         stateName(state), detail.empty() ? "none" : detail.c_str());
    if (state == app::StreamState::Error && !detail.empty()) setLastError(detail);
}

uint64_t SteamLinkStreamController::mediaTimestampNs() {
    const uint64_t now = steadyNowNs();
    uint64_t epoch = media_epoch_ns_.load(std::memory_order_acquire);
    if (epoch == 0 && media_epoch_ns_.compare_exchange_strong(
            epoch, now, std::memory_order_acq_rel, std::memory_order_acquire)) {
        epoch = now;
        lunar::diagnosticLog("steam-media", "media clock anchored");
    }
    return now >= epoch ? now - epoch : 0;
}

bool SteamLinkStreamController::startStream() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (state_.load() != app::StreamState::Idle) {
        setLastError("Steam Link stream is already active");
        return false;
    }
    if (!client_) {
        setState(app::StreamState::Error, "Steam Link client is unavailable");
        return false;
    }
    if (cancellation_.requested()) return false;
    session_connected_ = false;
    media_epoch_ns_ = 0;
    video_samples_ = 0;
    audio_samples_ = 0;
    audio_sequence_ = 0;
    rumble_generation_ = 0;
    guide_until_ns_ = 0;
    hid_announced_ = false;
    const auto touch_mode=pointer_.touch_mode;
    const auto gyro_mode=pointer_.gyro_mode;
    pointer_=SteamPointer{}; pointer_.touch_mode=touch_mode; pointer_.gyro_mode=gyro_mode;
    pointer_.fenceTouches(); // do not turn the Start button's release into a game click
    mouse_left_=mouse_right_=false;
    cursor_.reset(width_, height_);
    video_parameters_.clear();
    perf_.reset();
    setState(app::StreamState::Connecting, "Requesting Steam stream");
    lunar::persistentEventLog(
        "steam-stream", "start host=%s profile=%dx%d security_pin_len=%zu",
        host_.address.c_str(), width_, height_, security_pin_.size());

    auto wait = std::make_shared<StreamWaitState>();
    if (!client_->requestStreaming(host_, security_pin_, width_, height_,
            [wait](bool success, const SteamLinkStreamInfo& info,
                   const std::string& error) {
                std::lock_guard<std::mutex> wait_lock(wait->mutex);
                wait->done = true;
                wait->success = success;
                wait->info = info;
                wait->error = error;
                wait->condition.notify_one();
            })) {
        setState(app::StreamState::Error, client_->lastError());
        return false;
    }

    {
        std::unique_lock<std::mutex> wait_lock(wait->mutex);
        if (!cancellation_.wait(wait->condition, wait_lock, kStreamingWait,
                                [&wait]() { return wait->done; })) {
            wait_lock.unlock(); // cancel drains callbacks that may lock wait->mutex
            client_->cancelStreaming();
            setState(app::StreamState::Error,
                     "Timed out waiting for Steam streaming response");
            return false;
        }
        if (cancellation_.requested()) {
            wait_lock.unlock();
            client_->cancelStreaming();
            setState(app::StreamState::Disconnected, "Steam stream canceled");
            return false;
        }
        if (!wait->success) {
            setState(app::StreamState::Error,
                     wait->error.empty() ? "Steam streaming request failed" : wait->error);
            return false;
        }
        if (wait->info.session_key_len == 0 ||
            wait->info.session_key_len > sizeof(wait->info.session_key)) {
            setState(app::StreamState::Error, "Steam returned an invalid session key");
            return false;
        }
        if (wait->info.steam_id == 0) {
            // The Steam session authentication message carries this ID. It is
            // learned during pairing and must be available in this process.
            setState(app::StreamState::Error,
                     "Steam account ID unavailable; pair this host again");
            return false;
        }
    }

    if (!initializeMedia()) {
        setState(app::StreamState::Error, "Failed to initialize Steam media pipeline");
        return false;
    }
    startup_watchdog_.reset(steadyNowNs());
    if (!initializeSession(wait->info)) {
        if (media_) media_->shutdown();
        setState(app::StreamState::Error,
                 lastError().empty() ? "Failed to initialize Steam session" : lastError());
        return false;
    }
    input_pump_.start([this]() { update(); });
    lunar::persistentEventLog("steam-stream", "session started address=%s port=%u",
                              host_.address.c_str(), wait->info.address.port);
    return true;
}

bool SteamLinkStreamController::initializeMedia() {
    stream_backend_ = stream::StreamBackendProvider::createDefault();
    if (!stream_backend_) {
        setLastError("Steam media backend unavailable");
        return false;
    }
    media_ = std::make_unique<stream::MediaPipeline>(*stream_backend_);
    stream::MediaPipelineOptions options;
    options.video_path = stream::VideoPipelinePath::Xbox;
    options.video_codec = stream::VideoCodec::H264;
    options.video_backend = video_backend_;
    options.hold_non_target_startup_frames = true;
    options.video_scheduling = stream::VideoSchedulingMode::BoundedLowLatency;
    options.video_queue_limits.max_packets = 6;
    options.video_queue_limits.max_bytes = 8 * 1024 * 1024;
    options.video_queue_limits.max_age = std::chrono::milliseconds(100);
    media_->setVideoReadyCallback([this]() {
        if (cancellation_.requested()) return;
        auto expected = app::StreamState::Connecting;
        if (!state_.compare_exchange_strong(expected, app::StreamState::Streaming)) return;
        lunar::persistentEventLog("steam-media", "first video frame rendered");
    });
    lunar::diagnosticLog("steam-media", "initialize begin profile=%dx%d backend=%s",
                         width_, height_, stream::videoBackendName(video_backend_));
    if (!media_->initialize(width_, height_, &perf_, options)) {
        setLastError("Steam media pipeline initialization failed");
        media_.reset();
        return false;
    }
    lunar::diagnosticLog("steam-media", "initialize done");
    return true;
}

bool SteamLinkStreamController::initializeSession(const SteamLinkStreamInfo& info) {
    IHS_ClientConfig client_config{};
    // The client object owns the same identity used to request the stream. The
    // session API only needs a copy of the identity config.
    if (!client_->getSessionClientConfig(&client_config)) {
        setLastError("Steam session identity unavailable");
        return false;
    }
    IHS_SessionInfo session_info{};
    session_info.address = info.address;
    std::copy(info.session_key.begin(),
              info.session_key.begin() + info.session_key_len,
              session_info.sessionKey);
    session_info.sessionKeyLen = info.session_key_len;
    session_info.steamId = info.steam_id;

    session_ = IHS_SessionCreate(&client_config, &session_info);
    if (!session_) {
        setLastError("Steam session allocation failed");
        return false;
    }
    pad_state_ = std::make_shared<SteamPadState>();
    hid_announced_ = false;
    hid_provider_ = createSteamPadProvider(pad_state_);
    IHS_SessionHIDAddProvider(session_, hid_provider_);
    lunar::persistentEventLog("steam-hid", "registered generic Switch gamepad axes=6 buttons=16 report_bytes=48");
    static const IHS_StreamSessionCallbacks session_callbacks{
        &SteamLinkStreamController::onSessionInitialized,
        &SteamLinkStreamController::onSessionConnecting,
        &SteamLinkStreamController::onSessionConfiguring,
        &SteamLinkStreamController::onSessionConnected,
        &SteamLinkStreamController::onSessionDisconnected,
        &SteamLinkStreamController::onSessionFinalized};
    static const IHS_StreamAudioCallbacks audio_callbacks{
        &SteamLinkStreamController::onAudioStart,
        &SteamLinkStreamController::onAudioSubmit,
        &SteamLinkStreamController::onAudioStop};
    static const IHS_StreamVideoCallbacks video_callbacks{
        &SteamLinkStreamController::onVideoStart,
        &SteamLinkStreamController::onVideoSubmit,
        &SteamLinkStreamController::onVideoStop,
        &SteamLinkStreamController::onVideoCaptureSize,
        &SteamLinkStreamController::onVideoFramerate,
        &SteamLinkStreamController::onVideoBitrate,
        &SteamLinkStreamController::onVideoQuality,
        &SteamLinkStreamController::onVideoBitrateOverride};
    static const IHS_StreamInputCallbacks input_callbacks{
        &SteamLinkStreamController::onSetCursor,
        &SteamLinkStreamController::onDeleteCursor,
        &SteamLinkStreamController::onCursorImage,
        &SteamLinkStreamController::onShowCursor,
        &SteamLinkStreamController::onHideCursor,
        &SteamLinkStreamController::onCapsLock,
        &SteamLinkStreamController::onKeymap};
    IHS_SessionSetSessionCallbacks(session_, &session_callbacks, this);
    IHS_SessionSetAudioCallbacks(session_, &audio_callbacks, this);
    IHS_SessionSetVideoCallbacks(session_, &video_callbacks, this);
    IHS_SessionSetInputCallbacks(session_, &input_callbacks, this);
    IHS_SessionSetLogFunction(session_, &SteamLinkClient::logFunction);
    lunar::diagnosticLog("steam-session", "create address=%s port=%u steam_id=%llu key_len=%zu",
                         host_.address.c_str(), info.address.port,
                         static_cast<unsigned long long>(info.steam_id), info.session_key_len);
    if (!IHS_SessionConnect(session_)) {
        setLastError("Steam session worker failed to start");
        IHS_SessionDestroy(session_);
        session_ = nullptr;
        destroySteamPadProvider(hid_provider_);
        hid_provider_ = nullptr;
        return false;
    }
    return true;
}

void SteamLinkStreamController::requestStop() {
    cancellation_.request();
}

void SteamLinkStreamController::stopStream(bool set_disconnected) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    // update() never waits for this lock, so joining while holding it is safe.
    input_pump_.stop();
    sensors_.reset();
    if (session_) {
        if (mouse_left_) IHS_SessionSendMouseUp(session_, IHS_MOUSE_BUTTON_LEFT);
        if (mouse_right_) IHS_SessionSendMouseUp(session_, IHS_MOUSE_BUTTON_RIGHT);
    }
    mouse_left_=mouse_right_=false;
    if (rumble_) rumble_->stop();
    if (session_) {
        lunar::persistentEventLog("steam-stream", "session disconnect begin");
        closeSession(session_, [this](IHS_Session* session) {
            if (state_.load() != app::StreamState::Disconnected) {
                IHS_SessionDisconnect(session);
            }
        }, IHS_SessionThreadedJoin, IHS_SessionDestroy);
        lunar::persistentEventLog("steam-stream", "session disconnect done");
    }
    destroySteamPadProvider(hid_provider_);
    hid_provider_ = nullptr;
    pad_state_.reset();
    rumble_.reset();
    if (gamepad_) gamepad_->releaseCaptureButton();
    gamepad_.reset();
    if (media_) {
        media_->setVideoReadyCallback({});
        media_->shutdown();
        media_.reset();
    }
    stream_backend_.reset();
    cursor_.reset(width_, height_);
    session_connected_ = false;
    if (set_disconnected) setState(app::StreamState::Disconnected, "Stopped");
    else state_ = app::StreamState::Idle;
}

bool SteamLinkStreamController::resumeAfterForeground(CancelCallback cancel) {
    if (cancel && cancel()) return false;
    if (state_.load() == app::StreamState::Streaming && session_connected_.load()) {
        lunar::diagnosticLog("steam-stream", "foreground resume kept session");
        return true;
    }
    stopStream(false);
    if (cancel && cancel()) return false;
    return startStream();
}

void SteamLinkStreamController::update() {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || cancellation_.requested()) return;
    if (session_ && state_.load() == app::StreamState::Connecting) {
        const auto timeout = startup_watchdog_.expired(steadyNowNs());
        if (timeout != StartupWatchdog::Timeout::None) {
            const char* reason = timeout == StartupWatchdog::Timeout::Connection
                ? "Steam session connection timed out (15s); check host/network"
                : "Steam first video frame timed out (20s); check capture/encoding permissions and logs";
            auto expected = app::StreamState::Connecting;
            std::lock_guard<std::mutex> error_lock(error_mutex_);
            if (state_.compare_exchange_strong(expected, app::StreamState::Error)) {
                last_error_ = reason;
                lunar::persistentEventLog("steam-stream", "%s video_samples=%u audio_samples=%u",
                    reason, video_samples_.load(), audio_samples_.load());
                // Join/destruction remain on the owner's stop path, never on
                // the pump or an ihslib callback thread.
                IHS_SessionDisconnect(session_);
            }
        }
    }
    if (!session_ || !session_connected_.load() || state_.load() == app::StreamState::Error ||
        state_.load() == app::StreamState::Disconnected) return;
    if (!gamepad_) {
        gamepad_ = std::make_unique<input::GamepadReader>(input::ButtonMappingProfile::Steam);
        if (!gamepad_->initialize()) {
            lunar::persistentEventLog("steam-input", "gamepad initialize failed");
            gamepad_.reset();
            return;
        }
        rumble_ = std::make_unique<input::RumbleController>();
        if (!rumble_->initialize())
            lunar::persistentEventLog("steam-input", "rumble unavailable; input remains enabled");
        lunar::persistentEventLog("steam-input", "native generic gamepad input active");
        sensors_ = std::make_unique<SteamSensors>(pointer_.gyro_mode!=GyroMode::Off);
        lunar::persistentEventLog("steam-pointer", "touch_mode=%d gyro_mode=%d mouse gyro requires ZL",
            int(pointer_.touch_mode),int(pointer_.gyro_mode));
    }
    auto state = input_router_.route(gamepad_->read());
    const auto now = steadyNowNs();
    if (!hid_announced_ && hid_announce_log_.allow(now / 1000000)) {
        hid_announced_ = IHS_SessionHIDNotifyDeviceChange(session_);
        lunar::persistentEventLog("steam-hid", hid_announced_
            ? "gamepad device list sent" : "device list pending: host input not ready");
    }
    if (guide_requested_.exchange(false)) guide_until_ns_ = now + 150000000;
    state.guide = state.guide || now < guide_until_ns_;
    const bool game_input=input_router_.gameHasInput();
    const auto sensor_motion=sensors_->read();
    auto motion=sensor_motion;
    if (!game_input || pointer_.gyro_mode!=GyroMode::Native) motion={};
    pad_state_->publish(state,motion);
    const auto mouse_motion=pointer_.gyro_mode==GyroMode::Mouse ? sensor_motion : MotionSample{};
    auto pointer=pointer_.update(sensors_->touch(),mouse_motion,game_input,state.lt,now/1000000);
    if (pointer.absolute && IHS_SessionSendMousePosition(session_,pointer.x,pointer.y))
        cursor_.position(pointer.x, pointer.y);
    if ((pointer.dx || pointer.dy) && IHS_SessionSendMouseMovement(session_,pointer.dx,pointer.dy))
        cursor_.move(pointer.dx, pointer.dy);
    for(int i=0;i<std::abs(pointer.wheel);++i)
        IHS_SessionSendMouseWheel(session_,pointer.wheel>0?IHS_MOUSE_WHEEL_DOWN:IHS_MOUSE_WHEEL_UP);
    sendKeyTransition(pointer.left,mouse_left_,[&](bool down) {
        return down?IHS_SessionSendMouseDown(session_,IHS_MOUSE_BUTTON_LEFT)
                   :IHS_SessionSendMouseUp(session_,IHS_MOUSE_BUTTON_LEFT);
    });
    sendKeyTransition(pointer.right,mouse_right_,[&](bool down) {
        return down?IHS_SessionSendMouseDown(session_,IHS_MOUSE_BUTTON_RIGHT)
                   :IHS_SessionSendMouseUp(session_,IHS_MOUSE_BUTTON_RIGHT);
    });
    if (rumble_) {
        std::lock_guard<std::mutex> pad_lock(pad_state_->mutex);
        if (pad_state_->rumble_generation != rumble_generation_) {
            rumble_generation_ = pad_state_->rumble_generation;
            rumble_->setRumble(0, pad_state_->rumble_low / 65535.0f,
                pad_state_->rumble_high / 65535.0f, 0, 0,
                uint16_t(std::min(pad_state_->rumble_duration, uint32_t(65535))), 0, 0);
        }
        rumble_->setEnabled(input_router_.gameHasInput());
        rumble_->update();
    }
    if (analog_log_.allow(now / 1000000)) {
        bool host_sensors=false;
        { std::lock_guard<std::mutex> pad_lock(pad_state_->mutex); host_sensors=pad_state_->sensors_requested; }
        lunar::diagnosticLog("steam-pointer","game=%d motion=%d host_sensors=%d gyro=%.3f,%.3f,%.3f dx=%d dy=%d left=%d right=%d wheel=%d",
            int(game_input),int(motion.valid||mouse_motion.valid),int(host_sensors),
            sensor_motion.gyro[0],sensor_motion.gyro[1],sensor_motion.gyro[2],pointer.dx,pointer.dy,
            int(mouse_left_),int(mouse_right_),pointer.wheel);
        const auto report = encodeSteamPad(state);
        lunar::diagnosticLog("steam-hid", "host_opened=%d reports=%llu buttons=%04x lx=%d ly=%d rx=%d ry=%d lt=%u rt=%u rumble_commands=%llu",
            int(pad_state_->opened.load()), (unsigned long long)pad_state_->reports.load(),
            unsigned(report[16]) | (unsigned(report[17]) << 8),
            state.left_stick_x, state.left_stick_y, state.right_stick_x, state.right_stick_y,
            unsigned(report[8]) | (unsigned(report[9]) << 8),
            unsigned(report[10]) | (unsigned(report[11]) << 8),
            (unsigned long long)rumble_generation_);
    }
}

void SteamLinkStreamController::presentVideoFrame() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (media_) media_->presentVideoFrame();
}

void SteamLinkStreamController::setVideoPresentationSuspended(bool suspended) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (media_) media_->setVideoPresentationSuspended(suspended);
}

void SteamLinkStreamController::onSessionInitialized(IHS_Session*, void* context) {
    lunar::diagnosticLog("steam-session", "initialized");
    (void)context;
}
void SteamLinkStreamController::onSessionConnecting(IHS_Session*, void* context) {
    auto* self = static_cast<SteamLinkStreamController*>(context);
    if (self) lunar::diagnosticLog("steam-session", "connecting");
}
void SteamLinkStreamController::onSessionConfiguring(IHS_Session*, IHS_SessionConfig* config,
                                                     void* context) {
    auto* self = static_cast<SteamLinkStreamController*>(context);
    if (!config) return;
    config->enableAudio = true;
    config->enableHevc = false;
    if (self) lunar::diagnosticLog("steam-session", "configuring audio=1 hevc=0");
}
void SteamLinkStreamController::onSessionConnected(IHS_Session*, void* context) {
    auto* self = static_cast<SteamLinkStreamController*>(context);
    if (!self) return;
    self->startup_watchdog_.connected(steadyNowNs());
    self->session_connected_ = true;
    lunar::persistentEventLog("steam-session", "connected");
}
void SteamLinkStreamController::onSessionDisconnected(IHS_Session*, void* context) {
    auto* self = static_cast<SteamLinkStreamController*>(context);
    if (!self) return;
    self->session_connected_ = false;
    auto state = self->state_.load();
    while (state != app::StreamState::Error && state != app::StreamState::Disconnected &&
           !self->state_.compare_exchange_weak(state, app::StreamState::Disconnected)) {}
    lunar::persistentEventLog("steam-session", "disconnected state=%s", stateName(self->state_.load()));
}
void SteamLinkStreamController::onSessionFinalized(IHS_Session*, void*) {
    lunar::diagnosticLog("steam-session", "finalized");
}

int SteamLinkStreamController::onAudioStart(IHS_Session*, const IHS_StreamAudioConfig* config,
                                             void* context) {
    auto* self = static_cast<SteamLinkStreamController*>(context);
    if (!self || !config) return -1;
    lunar::diagnosticLog("steam-audio", "start codec=%d rate=%u channels=%u codec_data=%zu",
                         static_cast<int>(config->codec), config->frequency,
                         config->channels, config->codecDataLen);
    if (config->codec != IHS_StreamAudioCodecOpus || config->frequency != 48000 ||
        config->channels != 2) {
        lunar::persistentEventLog("steam-audio", "unsupported format codec=%d rate=%u channels=%u",
                                  static_cast<int>(config->codec), config->frequency,
                                  config->channels);
        return -1;
    }
    return 0;
}

int SteamLinkStreamController::onAudioSubmit(IHS_Session*, IHS_Buffer* data, void* context) {
    auto* self = static_cast<SteamLinkStreamController*>(context);
    if (!self || !self->media_ || !data || data->size == 0) return -1;
    const uint32_t count = self->audio_samples_.fetch_add(1) + 1;
    const uint64_t timestamp = self->mediaTimestampNs();
    self->media_->recordIncomingAudioPacket();
    const bool queued = self->media_->decodeAudioPacket(
        IHS_BufferPointer(data), data->size, self->audio_sequence_++, timestamp);
    if (count == 1 || count % 500 == 0) {
        lunar::diagnosticLog("steam-audio", "submit count=%u bytes=%zu queued=%d",
                             count, data->size, queued ? 1 : 0);
    }
    return queued ? 0 : -1;
}
void SteamLinkStreamController::onAudioStop(IHS_Session*, void* context) {
    auto* self = static_cast<SteamLinkStreamController*>(context);
    if (self) lunar::diagnosticLog("steam-audio", "stop packets=%u",
                                   self->audio_samples_.load());
}

int SteamLinkStreamController::onVideoStart(IHS_Session*, const IHS_StreamVideoConfig* config,
                                             void* context) {
    auto* self = static_cast<SteamLinkStreamController*>(context);
    if (!self || !config) return -1;
    lunar::persistentEventLog("steam-video", "start width=%u height=%u codec=%d codec_data=%zu",
                              config->width, config->height, static_cast<int>(config->codec),
                              config->codecDataLen);
    if (config->codec != IHS_StreamVideoCodecH264) return -1;
    self->cursor_.videoSize(config->width, config->height);
    if (!h264Parameters(config->codecData, config->codecDataLen, self->video_parameters_)) {
        lunar::persistentEventLog("steam-video", "invalid H264 codec data bytes=%zu", config->codecDataLen);
        return -1;
    }
    return 0;
}

IHS_StreamVideoSubmitResult SteamLinkStreamController::onVideoSubmit(
    IHS_Session*, IHS_Buffer* data, IHS_StreamVideoFrameFlag flags, void* context) {
    auto* self = static_cast<SteamLinkStreamController*>(context);
    if (!self || !self->media_ || !data || data->size == 0) {
        return IHS_StreamVideoSubmitError;
    }
    const uint32_t count = self->video_samples_.fetch_add(1) + 1;
    const uint64_t timestamp = self->mediaTimestampNs();
    self->media_->recordIncomingVideoSample(
        data->size, timestamp, 0);
    // Keep parameter sets and IDR in a single queue item: the low-latency
    // queue may evict a separate parameter-set packet before decoding it.
    const auto access_unit = h264AccessUnit(self->video_parameters_,
                                           IHS_BufferPointer(data), data->size);
    const bool queued = self->media_->decodeVideoPacket(
        access_unit.data(), access_unit.size(), timestamp);
    if (count == 1 || (count % 120 == 0)) {
        lunar::diagnosticLog("steam-video", "submit count=%u bytes=%zu keyframe=%d queued=%d",
                             count, data->size,
                             (flags & IHS_StreamVideoFrameKeyFrame) ? 1 : 0,
                             queued ? 1 : 0);
    }
    return queued ? IHS_StreamVideoSubmitOK : IHS_StreamVideoSubmitReportLost;
}
void SteamLinkStreamController::onVideoStop(IHS_Session*, void* context) {
    auto* self = static_cast<SteamLinkStreamController*>(context);
    if (self) lunar::persistentEventLog("steam-video", "stop samples=%u",
                                       self->video_samples_.load());
}
int SteamLinkStreamController::onVideoCaptureSize(IHS_Session*, int width, int height, void* context) {
    if (auto* self = static_cast<SteamLinkStreamController*>(context))
        self->cursor_.captureSize(width, height);
    lunar::diagnosticLog("steam-video", "capture size=%dx%d", width, height);
    return 0;
}
void SteamLinkStreamController::onVideoFramerate(IHS_Session*, uint32_t numerator,
                                                  uint32_t denominator, uint32_t reasons, void*) {
    lunar::diagnosticLog("steam-video", "target framerate=%u/%u reasons=0x%x",
                         numerator, denominator, reasons);
}
void SteamLinkStreamController::onVideoBitrate(IHS_Session*, int32_t bitrate, void*) {
    lunar::diagnosticLog("steam-video", "target bitrate=%d", bitrate);
}
void SteamLinkStreamController::onVideoQuality(IHS_Session*, int32_t value, void*) {
    lunar::diagnosticLog("steam-video", "quality override=%d", value);
}
void SteamLinkStreamController::onVideoBitrateOverride(IHS_Session*, int32_t value, void*) {
    lunar::diagnosticLog("steam-video", "bitrate override=%d", value);
}

bool SteamLinkStreamController::onSetCursor(IHS_Session*, uint64_t id, void* context) {
    return static_cast<SteamLinkStreamController*>(context)->cursor_.select(id);
}
bool SteamLinkStreamController::onDeleteCursor(IHS_Session*, uint64_t id, void* context) {
    static_cast<SteamLinkStreamController*>(context)->cursor_.erase(id);
    return true;
}
void SteamLinkStreamController::onCursorImage(IHS_Session*, const IHS_StreamInputCursorImage* image, void* context) {
    if (!image) return;
    const bool accepted = static_cast<SteamLinkStreamController*>(context)->cursor_.image(
        image->cursorId, image->width, image->height, image->hotX, image->hotY, image->image, image->imageLen);
    lunar::diagnosticLog("steam-input", "cursor image accepted=%d id=%llu %dx%d bytes=%zu", int(accepted),
                                   static_cast<unsigned long long>(image->cursorId), image->width,
                                   image->height, image->imageLen);
}
void SteamLinkStreamController::onShowCursor(IHS_Session*, float x, float y, void* context) {
    static_cast<SteamLinkStreamController*>(context)->cursor_.show(x, y);
    lunar::diagnosticLog("steam-input", "show cursor x=%.3f y=%.3f", x, y);
}
void SteamLinkStreamController::onHideCursor(IHS_Session*, void* context) {
    static_cast<SteamLinkStreamController*>(context)->cursor_.hide();
    lunar::diagnosticLog("steam-input", "hide cursor");
}
void SteamLinkStreamController::onCapsLock(IHS_Session*, bool pressed, void*) {
    lunar::diagnosticLog("steam-input", "caps lock=%d", pressed ? 1 : 0);
}
void SteamLinkStreamController::onKeymap(IHS_Session*, const IHS_KeymapEntry*, size_t count, void*) {
    lunar::diagnosticLog("steam-input", "keymap entries=%zu", count);
}

} // namespace lunar::steamlink

#endif
