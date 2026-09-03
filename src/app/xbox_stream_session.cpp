#include "xbox_stream_session.h"
#include "xbox_latency_policy.h"
#include "adaptive_bitrate_controller.h"
#include "video_recovery_request_policy.h"
#include "video_watchdog_policy.h"
#include "xbox_ice_preferences.h"
#include "../diagnostics.h"
#include "../webrtc/video_jitter_policy.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <cstdio>

#ifndef LUNARNX_VERSION
#define LUNARNX_VERSION "unknown"
#endif
#ifndef LUNARNX_GIT_COMMIT
#define LUNARNX_GIT_COMMIT "unknown"
#endif

namespace lunar::app {

using lunar::stream::videoRenderStageName;

namespace {

constexpr std::chrono::milliseconds kIceStableWindow{800};
constexpr std::chrono::milliseconds kIceGatherTimeout{5000};
constexpr std::chrono::milliseconds kNetworkPumpInterval{2};
// Green-NX sends one complete current-state frame every 8 ms. Lost or locally
// backpressured packets are repaired by the next fresh snapshot, never by
// replaying an older transition.
constexpr std::chrono::milliseconds kInputSampleInterval{8};
constexpr std::chrono::milliseconds kRumbleUpdateInterval{16};
constexpr std::chrono::seconds kDataChannelTimeout{45};
constexpr std::chrono::seconds kMediaStartupTimeout{15};
constexpr std::chrono::seconds kStartupKeyframeRetryInterval{1};
constexpr std::chrono::seconds kReceiverFeedbackInterval{1};
constexpr std::chrono::milliseconds kMediaHealthPollInterval{50};
constexpr std::chrono::milliseconds kVideoRtpStallTimeout{3000};
constexpr std::chrono::milliseconds kVideoDecodeStallTimeout{2000};
constexpr std::chrono::milliseconds kVideoPresentStallTimeout{1000};
constexpr std::chrono::milliseconds kVideoHealthRecoveryCooldown{2000};
// Preserve the old 8 x 16 ms Guide pulse while sampling input at 8 ms.
constexpr int kGuidePulseFrames = 16;
constexpr uint32_t kRecoveryMissingPacketsThreshold = 12;
constexpr uint32_t kRecoveryCorruptFramesThreshold = 4;

#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
uint64_t latencyAverageUs(uint64_t total, uint32_t samples) {
    return samples == 0 ? 0 : total / samples;
}
#endif

const char* iceCandidateTypeName(uint8_t type) {
    switch (type) {
        case 0: return "host";
        case 1: return "srflx";
        case 2: return "prflx";
        case 3: return "relay";
        default: return "unknown";
    }
}

void notify(const std::function<void(const std::string&)>& callback,
            const std::string& message) {
    if (callback) {
        callback(message);
    }
}

void rememberSuccessfulIcePreferences(WebRtcTransport& transport,
                                      const StreamProfile& profile) {
    XboxIcePreference preference;
    preference.preferred_stun_url = transport.successfulIceServerUrl();
    if (profile.type == SessionType::Home) {
        const auto stats = transport.getMediaStats();
        if (stats.ice_pair_selected && stats.ice_remote_address[0] != '\0' &&
            stats.ice_remote_port > 0) {
            preference.remote_address = stats.ice_remote_address;
            preference.remote_port = stats.ice_remote_port;
        }
    }
    if (preference.preferred_stun_url.empty() &&
        !preference.hasHomeRoute()) {
        return;
    }

    const std::string server_id = profile.type == SessionType::Home
        ? profile.server_id
        : std::string{};
    const bool saved = XboxIcePreferenceStore().save(server_id, preference);
    lunar::diagnosticLog(
        "xbox-stream",
        "ICE preference save result=%s stun=%s remote=%s:%d",
        saved ? "true" : "false",
        preference.preferred_stun_url.empty()
            ? "none"
            : preference.preferred_stun_url.c_str(),
        preference.remote_address.empty()
            ? "none"
            : preference.remote_address.c_str(),
        preference.remote_port);
}

std::string offerForProfile(std::string offer, const StreamProfile& profile) {
    // libpeer's base offer intentionally matches the accepted 720p native
    // template. For a 1080p profile, expand only the H.264 decode limits; the
    // receiver capability/bitrate itself is declared on the message channel.
    if (profile.width >= 1920 && profile.height >= 1080) {
        const auto max_fs = offer.find("max-fs=3600");
        if (max_fs != std::string::npos) {
            offer.replace(max_fs + 7, 4, "8160");
        }
        const auto max_mbps = offer.find("max-mbps=108000");
        if (max_mbps != std::string::npos) {
            offer.replace(max_mbps + 9, 6, "489600");
        }
    }
    // Home-console agents use the level-3.2 variant of the same baseline
    // template. Cloud keeps the native 42e01f profile-level-id.
    if (profile.type == SessionType::Home && profile.height <= 720) {
        const auto level = offer.find("profile-level-id=42e01f");
        if (level != std::string::npos) {
            offer.replace(level + 17, 6, "42e020");
        }
    }
    return offer;
}

bool isNonRetryableStartError(std::string error) {
    std::transform(error.begin(), error.end(), error.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return error.find("cancel") != std::string::npos ||
           error.find("unsupported") != std::string::npos ||
           error.find("msal") != std::string::npos ||
           error.find("token") != std::string::npos ||
           error.find("auth") != std::string::npos;
}

} // namespace

XboxStreamSession::XboxStreamSession(XboxSessionClient& session_client,
                                     WebRtcTransport& transport,
                                     XboxChannelManager& channels,
                                     stream::MediaPipeline& media,
                                     input::GamepadReader& gamepad,
                                     input::XInputEncoder& xinput,
                                     input::RumbleController& rumble,
                                     input::StreamInputRouter& input_router,
                                     stream::PerfStats& perf)
    : session_client_(session_client),
      transport_(transport),
      channels_(channels),
      media_(media),
      gamepad_(gamepad),
      xinput_(xinput),
      rumble_(rumble),
      input_router_(input_router),
      perf_(perf) {}

XboxStreamSession::~XboxStreamSession() {
    stop();
}

bool XboxStreamSession::start(const StreamProfile& profile,
                              const stream::MediaPipelineOptions& media_options,
                              RuntimeCallbacks callbacks) {
    stop();
    stop_requested_ = false;
    control_recovery_requested_ = false;
    media_startup_ready_ = false;
    channels_.reset();
    transport_.setVideoJitterMode(
        profile.type == SessionType::Cloud
            ? webrtc::VideoJitterMode::Cloud
            : webrtc::VideoJitterMode::Home);
    transport_.setPreferredIceServerUrl(profile.preferred_stun_url);

    auto cancel = [this, callbacks]() { return isCancelled(callbacks); };
    auto sleep = [this, callbacks](std::chrono::milliseconds duration) {
        return sleepUnlessCancelled(duration, callbacks);
    };

    notify(callbacks.on_status, "Cleaning up old sessions...");
    session_client_.cleanupStaleSessions(profile, cancel);
    if (isCancelled(callbacks)) {
        return cancelStart(callbacks);
    }

    const bool is_cloud = profile.type == SessionType::Cloud;
    const int max_start_attempts = is_cloud ? 2 : 4;
    const auto retry_delay = is_cloud ? std::chrono::seconds(3)
                                      : std::chrono::seconds(5);
    ProvisionedSession session;
    std::string start_error = "Session creation failed";
    bool negotiated = false;

    for (int attempt = 0; attempt < max_start_attempts; ++attempt) {
        if (attempt > 0) {
            notify(callbacks.on_status,
                   std::string(is_cloud ? "Retrying cloud session ("
                                        : "Retrying console session (") +
                       std::to_string(attempt + 1) + "/" +
                       std::to_string(max_start_attempts) + ")...");
            if (!sleep(retry_delay)) {
                return cancelStart(callbacks);
            }
            session_client_.cleanupStaleSessions(profile, cancel);
            if (isCancelled(callbacks)) {
                return cancelStart(callbacks);
            }
        }

        session = session_client_.createAndWait(
            profile, cancel, callbacks.on_status, sleep);
        if (session.status == SessionStartStatus::Cancelled) {
            if (!session.session_id.empty()) {
                session_client_.deleteSessionAsync(session.session_id);
            }
            return cancelStart(callbacks);
        }
        if (session.status == SessionStartStatus::Unsupported) {
            if (!session.session_id.empty()) {
                session_client_.deleteSessionAsync(session.session_id);
            }
            return failStart(session.error.empty() ? "Session is unsupported"
                                                   : session.error,
                             callbacks);
        }
        if (session.status == SessionStartStatus::Failed) {
            start_error = session.error.empty() ? "Session creation failed"
                                                : session.error;
            if (!session.session_id.empty()) {
                session_client_.deleteSessionAsync(session.session_id);
            }
            lunar::diagnosticLog("xbox-stream",
                                 "Session start attempt failed attempt=%d/%d error=%s",
                                 attempt + 1,
                                 max_start_attempts,
                                 start_error.c_str());
            const bool retry_waking_home =
                !is_cloud && start_error.find("AgentCommandError") != std::string::npos;
            if (attempt + 1 >= max_start_attempts ||
                !retry_waking_home ||
                isNonRetryableStartError(start_error)) {
                return failStart(start_error, callbacks);
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            session_id_ = session.session_id;
        }
        notify(callbacks.on_session_id, session.session_id);

        notify(callbacks.on_status, "Setting up WebRTC...");
        if (isCancelled(callbacks)) {
            return cancelStart(callbacks);
        }
        if (!transport_.initialize()) {
            start_error = "WebRTC init failed";
            lunar::diagnosticLog("xbox-stream",
                                 "WebRTC transport init failed attempt=%d/%d",
                                 attempt + 1,
                                 max_start_attempts);
        } else {
            rumble_.initialize();
            notify(callbacks.on_status, "Exchanging SDP...");
            const bool webrtc_negotiated =
                negotiateWebRtc(profile, session.session_id, callbacks);
            if (!webrtc_negotiated) {
                start_error = "WebRTC negotiation failed";
            } else {
                perf_.reset();
                transport_.setCallbacks(createPeerCallbacks());
                lunar::diagnosticLog("xbox-stream", "Peer callbacks installed");
                transport_.setMediaEnabled(false);

                notify(callbacks.on_status, "Establishing data channel...");
                negotiated = transport_.waitDataChannels(kDataChannelTimeout, cancel);
                if (!negotiated) {
                    start_error = "Data channel timeout";
                }
            }
        }

        if (negotiated) {
            break;
        }
        if (isCancelled(callbacks)) {
            return cancelStart(callbacks);
        }

        transport_.disconnect();
        channels_.reset();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            session_id_.clear();
        }
        session_client_.deleteSessionAsync(session.session_id);
        session.session_id.clear();
        lunar::diagnosticLog("xbox-stream",
                             "WebRTC start attempt failed attempt=%d/%d error=%s",
                             attempt + 1,
                             max_start_attempts,
                             start_error.c_str());
        if (attempt + 1 >= max_start_attempts) {
            return failStart(start_error, callbacks);
        }
    }

    if (!negotiated) {
        return failStart(start_error, callbacks);
    }
    // The data channel proves ICE/DTLS/SCTP are usable. Capture the selected
    // pair before the realtime owner thread starts pumping this PeerConnection.
    rememberSuccessfulIcePreferences(transport_, profile);

    notify(callbacks.on_status, "Initializing media pipeline...");
    if (isCancelled(callbacks)) {
        return cancelStart(callbacks);
    }
    lunar::diagnosticLog("xbox-stream", "Media init begin width=%d height=%d",
                         profile.width,
                         profile.height);
    auto effective_media_options = media_options;
    const bool is_cloud_profile = profile.type == SessionType::Cloud;
    effective_media_options.video_presentation_mode =
        xboxVideoPresentationMode(is_cloud_profile, {});
    effective_media_options.video_decode_catch_up_mode =
        xboxVideoDecodeCatchUpMode(is_cloud_profile);
    effective_media_options.audio_latency_mode =
        xboxAudioLatencyMode(is_cloud_profile);
    if (!media_.initialize(profile.width, profile.height, &perf_,
                           effective_media_options)) {
        lunar::diagnosticLog("xbox-stream", "Media init failed");
        return failStart("Media init failed", callbacks);
    }
    lunar::diagnosticLog("xbox-stream", "Media init done");

    lunar::diagnosticLog("xbox-stream", "Gamepad init begin");
    if (!gamepad_.initialize()) {
        lunar::diagnosticLog("xbox-stream", "Gamepad init failed");
        return failStart("Media/input init failed", callbacks);
    }
    lunar::diagnosticLog("xbox-stream", "Gamepad init done");

    const int keep_alive_seconds =
        session.config.keep_alive_seconds > 0 ? session.config.keep_alive_seconds : 300;
    perf_.recordKeepAliveInterval(static_cast<uint32_t>(keep_alive_seconds));

    streaming_ = true;
    input_delivery_ready_ = false;
    lunar::startDropDiagnosticWriter();
    lunar::latencyDiagnosticLog(
        "session",
        "phase=start version=%s commit=%s type=%s width=%d height=%d "
        "fps=%d requested_kbps=%d backend=%s presentation=%s catchup=%s audio=%s "
        "audio_capacity_ms=%zu app_diag=%d drop_diag=%d "
        "report_ms=1000 writer=persistent-async flush_ms=1000",
        LUNARNX_VERSION,
        LUNARNX_GIT_COMMIT,
        profile.type == SessionType::Cloud ? "cloud" : "home",
        profile.width,
        profile.height,
        profile.fps,
        streamProfileBitrateKbps(profile),
        stream::videoBackendName(effective_media_options.video_backend),
        stream::videoPresentationModeName(
            effective_media_options.video_presentation_mode),
        stream::videoDecodeCatchUpModeName(
            effective_media_options.video_decode_catch_up_mode),
        stream::audioLatencyModeName(
            effective_media_options.audio_latency_mode),
        stream::audioBufferCapacityMs(
            effective_media_options.audio_latency_mode),
        LUNARNX_DIAGNOSTIC_LOG,
        LUNARNX_DROP_DIAGNOSTIC_LOG);
    lunar::setCloud1080CrashProbeEnabled(
        profile.type == SessionType::Cloud &&
        profile.width >= 1920 && profile.height >= 1080);
    lunar::cloud1080CrashProbeLog(
        "crash-probe",
        "DEBUG-c1080 phase=session-start type=cloud width=%d height=%d "
        "fps=%d bitrate_kbps=%d backend=%s",
        profile.width,
        profile.height,
        profile.fps,
        streamProfileBitrateKbps(profile),
        stream::videoBackendName(media_options.video_backend));
    try {
        startInputLoop(callbacks);
        std::lock_guard<std::mutex> lock(state_mutex_);
        lunar::diagnosticLog("xbox-stream", "Stream thread create begin");
        stream_thread_ = std::thread(&XboxStreamSession::runLoop,
                                     this,
                                     profile,
                                     session.session_id,
                                     keep_alive_seconds,
                                     callbacks);
        lunar::diagnosticLog("xbox-stream", "Stream thread create done");
        lunar::diagnosticLog("xbox-stream", "Control thread create begin");
        control_thread_ = std::thread(&XboxStreamSession::controlLoop,
                                      this,
                                      session.session_id,
                                      keep_alive_seconds,
                                      callbacks);
        lunar::diagnosticLog("xbox-stream", "Control thread create done");
    } catch (const std::exception& e) {
        lunar::diagnosticLog("xbox-stream", "Worker thread create failed: %s", e.what());
        stop(true);
        notify(callbacks.on_error, std::string("Stream worker startup failed: ") + e.what());
        return false;
    } catch (...) {
        lunar::diagnosticLog("xbox-stream", "Worker thread create failed: unknown exception");
        stop(true);
        notify(callbacks.on_error, "Stream worker startup failed: unknown exception");
        return false;
    }

    notify(callbacks.on_status, "Starting media stream...");
    const auto media_startup_begin = std::chrono::steady_clock::now();
    const auto media_startup_deadline =
        media_startup_begin + kMediaStartupTimeout;
    lunar::persistentEventLog(
        "xbox-startup",
        "phase=control-wait timeout_ms=%lld connected=%s data_ready=%s",
        static_cast<long long>(kMediaStartupTimeout.count() * 1000),
        transport_.isConnected() ? "true" : "false",
        transport_.isDataChannelReady() ? "true" : "false");
    while (!media_startup_ready_.load() && streaming_.load() &&
           !isCancelled(callbacks) &&
           std::chrono::steady_clock::now() < media_startup_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (!media_startup_ready_.load()) {
        const bool cancelled = isCancelled(callbacks);
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - media_startup_begin).count();
        lunar::persistentEventLog(
            "xbox-startup",
            "phase=media-startup-timeout elapsed_ms=%lld cancelled=%s "
            "streaming=%s connected=%s data_ready=%s video_rtp=%u audio_rtp=%u "
            "rtp_queue_high=%u rtp_queue_drops=%u",
            static_cast<long long>(elapsed_ms),
            cancelled ? "true" : "false",
            streaming_.load() ? "true" : "false",
            transport_.isConnected() ? "true" : "false",
            transport_.isDataChannelReady() ? "true" : "false",
            perf_.rtp_video_packets.load(),
            perf_.rtp_audio_packets.load(),
            perf_.rtp_queue_high_watermark.load(),
            perf_.rtp_queue_drops.load());
        stop(true);
        if (cancelled) {
            notify(callbacks.on_cancelled, "Connection cancelled");
        } else {
            notify(callbacks.on_error,
                   "Media startup timed out before the Xbox control handshake completed.");
        }
        return false;
    }

    lunar::persistentEventLog(
        "xbox-startup", "phase=media-ready elapsed_ms=%lld",
        static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - media_startup_begin).count()));
    if (callbacks.on_streaming) {
        callbacks.on_streaming();
    }

    return true;
}

void XboxStreamSession::stop(bool delete_session) {
    const auto stop_started_at = std::chrono::steady_clock::now();
    lunar::persistentEventLog("xbox-stream",
                              "stop begin delete_session=%s",
                              delete_session ? "true" : "false");
    stop_requested_ = true;
    streaming_ = false;
    input_loop_stop_ = true;
    input_delivery_ready_ = false;
    media_startup_ready_ = false;
    control_recovery_requested_ = false;
    control_cv_.notify_all();

    std::thread stream_thread_to_join;
    std::thread control_thread_to_join;
    std::thread input_thread_to_join;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto current_thread = std::this_thread::get_id();
        const bool called_from_stream_thread =
            stream_thread_.joinable() && stream_thread_.get_id() == current_thread;
        const bool called_from_control_thread =
            control_thread_.joinable() && control_thread_.get_id() == current_thread;
        const bool called_from_input_thread =
            input_thread_.joinable() && input_thread_.get_id() == current_thread;
        if (called_from_stream_thread || called_from_control_thread ||
            called_from_input_thread) {
            lunar::diagnosticLog(
                "xbox-stream",
                "Worker requested stop; owner must join before cleanup");
            return;
        }
        if (stream_thread_.joinable()) {
            stream_thread_to_join = std::move(stream_thread_);
        }
        if (control_thread_.joinable()) {
            control_thread_to_join = std::move(control_thread_);
        }
        if (input_thread_.joinable()) {
            input_thread_to_join = std::move(input_thread_);
        }
    }
    const auto join_started_at = std::chrono::steady_clock::now();
    lunar::persistentEventLog("xbox-stream", "stop phase=worker-join begin");
    if (stream_thread_to_join.joinable()) {
        stream_thread_to_join.join();
    }
    if (control_thread_to_join.joinable()) {
        control_thread_to_join.join();
    }
    if (input_thread_to_join.joinable()) {
        input_thread_to_join.join();
    }
    const auto join_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - join_started_at);
    lunar::persistentEventLog(
        "xbox-stream", "stop phase=worker-join done elapsed_ms=%lld slow=%s",
        static_cast<long long>(join_elapsed.count()),
        join_elapsed >= std::chrono::seconds(3) ? "true" : "false");

    cleanupResources(delete_session);
    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stop_started_at);
    lunar::persistentEventLog(
        "xbox-stream", "stop complete total_ms=%lld slow=%s",
        static_cast<long long>(total.count()),
        total >= std::chrono::seconds(3) ? "true" : "false");
}

std::string XboxStreamSession::sessionId() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return session_id_;
}

void XboxStreamSession::startInputLoop(RuntimeCallbacks callbacks) {
    stopInputLoop();
    input_accumulator_.reset();
    input_loop_stop_ = false;
    input_thread_ = std::thread([this, callbacks]() {
        int guide_pulse_frames_remaining = 0;
        bool guide_release_pending = false;
        auto next_input_tick = std::chrono::steady_clock::now();
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        auto previous_sample_started = std::chrono::steady_clock::time_point{};
#endif

        while (!input_loop_stop_.load() && streaming_.load() &&
               !isCancelled(callbacks)) {
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
            const auto sample_started = std::chrono::steady_clock::now();
#endif
            try {
                sampleInput(callbacks,
                            guide_pulse_frames_remaining,
                            guide_release_pending);
            } catch (const std::exception& e) {
                lunar::diagnosticLog("xbox-input", "input sample exception: %s", e.what());
            } catch (...) {
                lunar::diagnosticLog("xbox-input", "input sample unknown exception");
            }
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
            const auto sample_finished = std::chrono::steady_clock::now();
            const uint64_t sample_gap_us =
                previous_sample_started.time_since_epoch().count() == 0
                    ? 0
                    : static_cast<uint64_t>(
                          std::chrono::duration_cast<std::chrono::microseconds>(
                              sample_started - previous_sample_started).count());
            const uint64_t sample_read_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    sample_finished - sample_started).count());
            perf_.recordInputSample(sample_gap_us, sample_read_us);
            previous_sample_started = sample_started;
#endif

            next_input_tick += kInputSampleInterval;
            const auto now = std::chrono::steady_clock::now();
            if (next_input_tick <= now) {
                next_input_tick = now + kInputSampleInterval;
            }
            std::this_thread::sleep_until(next_input_tick);
        }
    });
}

void XboxStreamSession::stopInputLoop() {
    input_loop_stop_ = true;
    if (input_thread_.joinable() &&
        input_thread_.get_id() != std::this_thread::get_id()) {
        input_thread_.join();
    }
}

void XboxStreamSession::sampleInput(
    const RuntimeCallbacks& callbacks,
    int& guide_pulse_frames_remaining,
    bool& guide_release_pending) {
    input::GamepadState gamepad_state{};
    try {
        gamepad_state = gamepad_.read();
    } catch (...) {
        // Keep zeroed input if HID polling fails for this sample.
    }
    if (input_delivery_ready_.load() && callbacks.consume_guide_button &&
        callbacks.consume_guide_button()) {
        guide_pulse_frames_remaining = kGuidePulseFrames;
        guide_release_pending = true;
    }
    if (guide_pulse_frames_remaining > 0) {
        // The realtime input channel may drop a single frame. Hold Nexus
        // briefly, then follow it with an explicit release frame.
        gamepad_state = {};
        gamepad_state.guide = true;
        guide_pulse_frames_remaining--;
    } else if (guide_release_pending) {
        gamepad_state = {};
        guide_release_pending = false;
    }
    gamepad_state = input_router_.route(gamepad_state);
    input_accumulator_.publish(gamepad_state, input_delivery_ready_.load());
}

void XboxStreamSession::prepareInputForReconnect() {
    input_delivery_ready_ = false;
    input_accumulator_.prepareForReconnect();
    lunar::persistentEventLog("xbox-input", "input-reconnect-resync");
}

bool XboxStreamSession::isCancelled(const RuntimeCallbacks& callbacks) const {
    return stop_requested_.load() ||
           (callbacks.external_cancel && callbacks.external_cancel());
}

bool XboxStreamSession::sleepUnlessCancelled(
    std::chrono::milliseconds duration,
    const RuntimeCallbacks& callbacks) const {
    auto slept = std::chrono::milliseconds(0);
    while (slept < duration) {
        if (isCancelled(callbacks)) {
            return false;
        }
        const auto step = std::min(std::chrono::milliseconds(50), duration - slept);
        std::this_thread::sleep_for(step);
        slept += step;
    }
    return !isCancelled(callbacks);
}

bool XboxStreamSession::sleepUntilCancelled(
    std::chrono::steady_clock::time_point deadline,
    const RuntimeCallbacks& callbacks) const {
    constexpr std::chrono::milliseconds kCancellationPollInterval{4};
    while (!isCancelled(callbacks)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return true;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        std::this_thread::sleep_for(
            std::min(kCancellationPollInterval,
                     std::max(std::chrono::milliseconds(1), remaining)));
    }
    return false;
}

bool XboxStreamSession::negotiateWebRtc(const StreamProfile& profile,
                                        const std::string& session_id,
                                        const RuntimeCallbacks& callbacks) {
    auto cancel = [this, callbacks]() { return isCancelled(callbacks); };
    auto sleep = [this, callbacks](std::chrono::milliseconds duration) {
        return sleepUnlessCancelled(duration, callbacks);
    };

    const std::string raw_offer = transport_.createOffer();
    const std::string offer = offerForProfile(raw_offer, profile);
    if (offer.empty() || isCancelled(callbacks)) {
        lunar::diagnosticLog("xbox-stream", "Create WebRTC offer failed or cancelled");
        return false;
    }
    lunar::diagnosticLog("webrtc-sdp",
                         "local offer profile width=%d height=%d bitrate_kbps=%d raw_len=%zu munged_len=%zu max_fs=%s max_mbps=%s",
                         profile.width,
                         profile.height,
                         streamProfileBitrateKbps(profile),
                         raw_offer.size(),
                         offer.size(),
                         offer.find("max-fs=8160") != std::string::npos ? "8160" : "3600",
                         offer.find("max-mbps=489600") != std::string::npos ? "489600" : "108000");

    std::string answer;
    if (!session_client_.exchangeSdpAnswer(session_id, offer, answer, cancel, sleep)) {
        lunar::diagnosticLog("xbox-stream", "SDP answer exchange failed");
        return false;
    }
    if (answer.empty() || isCancelled(callbacks)) {
        lunar::diagnosticLog("xbox-stream", "SDP answer empty or cancelled");
        return false;
    }
    transport_.setRemoteAnswer(answer);

    notify(callbacks.on_status, "Collecting ICE candidates...");
    const auto local_candidates = transport_.gatherLocalCandidates(
        kIceStableWindow,
        kIceGatherTimeout,
        cancel);
    if (isCancelled(callbacks)) {
        return false;
    }
    const std::string ice_ufrag =
        IceCandidateProcessor::usernameFragmentFromSdp(offer);
    if (!session_client_.sendIceCandidates(session_id,
                                           local_candidates,
                                           cancel,
                                           ice_ufrag)) {
        lunar::diagnosticLog("xbox-stream", "Sending ICE candidates failed count=%zu",
                             local_candidates.size());
        return false;
    }
    const auto remote_candidates =
        session_client_.getIceCandidates(session_id, profile, cancel, sleep);
    if (isCancelled(callbacks)) {
        return false;
    }
    if (remote_candidates.empty()) {
        lunar::diagnosticLog("xbox-stream", "Server returned no usable ICE candidates");
        return false;
    }
    transport_.addRemoteCandidates(remote_candidates);
    return true;
}

void XboxStreamSession::prepareFreshSessionReconnect(const char* reason) {
    const char* safe_reason = reason ? reason : "connection-state";
    lunar::persistentEventLog(
        "xbox-stream", "fresh-reconnect phase=begin reason=%s", safe_reason);
    prepareInputForReconnect();

    lunar::persistentEventLog(
        "xbox-stream", "fresh-reconnect phase=transport-quiesce-begin reason=%s",
        safe_reason);
    transport_.setMediaEnabled(false);
    transport_.disconnect();
    channels_.reset();
    lunar::persistentEventLog(
        "xbox-stream", "fresh-reconnect phase=transport-quiesce-done reason=%s",
        safe_reason);

    // No libpeer callback can enqueue another old-source packet after the peer
    // is destroyed. Advance media epochs only after that callback boundary.
    lunar::persistentEventLog(
        "xbox-stream", "fresh-reconnect phase=media-source-reset-begin reason=%s",
        safe_reason);
    media_.prepareForNewMediaSource(safe_reason);
    lunar::persistentEventLog(
        "xbox-stream", "fresh-reconnect phase=media-source-reset-done reason=%s",
        safe_reason);
}

bool XboxStreamSession::reconnectWithFreshSession(
    const StreamProfile& profile,
    std::string& session_id,
    int& keep_alive_seconds,
    const RuntimeCallbacks& callbacks,
    bool& reconnect_prepared,
    const std::string& reconnect_reason) {
    auto cancel = [this, callbacks]() { return isCancelled(callbacks); };
    auto sleep = [this, callbacks](std::chrono::milliseconds duration) {
        return sleepUnlessCancelled(duration, callbacks);
    };

    const std::string old_session_id = session_id;
    if (!reconnect_prepared) {
        prepareFreshSessionReconnect(reconnect_reason.c_str());
        reconnect_prepared = true;
    }
    session_id.clear();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        session_id_.clear();
    }
    if (!old_session_id.empty()) {
        session_client_.deleteSessionAsync(old_session_id);
    }

    notify(callbacks.on_status, "Recreating streaming session...");
    {
        std::lock_guard<std::mutex> api_lock(session_api_mutex_);
        session_client_.cleanupStaleSessions(profile, cancel);
    }
    if (isCancelled(callbacks)) return false;

    ProvisionedSession session;
    {
        std::lock_guard<std::mutex> api_lock(session_api_mutex_);
        session = session_client_.createAndWait(
            profile, cancel, callbacks.on_status, sleep);
    }
    if (session.status != SessionStartStatus::Ok || session.session_id.empty()) {
        if (!session.session_id.empty()) {
            session_client_.deleteSessionAsync(session.session_id);
        }
        lunar::diagnosticLog(
            "xbox-stream", "Fresh session creation failed status=%d error=%s",
            static_cast<int>(session.status), session.error.c_str());
        return false;
    }
    if (isCancelled(callbacks)) {
        session_client_.deleteSessionAsync(session.session_id);
        return false;
    }

    session_id = session.session_id;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        session_id_ = session_id;
    }
    notify(callbacks.on_session_id, session_id);
    keep_alive_seconds = session.config.keep_alive_seconds > 0
        ? session.config.keep_alive_seconds
        : 300;
    perf_.recordKeepAliveInterval(static_cast<uint32_t>(keep_alive_seconds));

    if (!transport_.initialize()) {
        lunar::diagnosticLog("xbox-stream", "Fresh WebRTC transport init failed");
        transport_.disconnect();
        session_client_.deleteSessionAsync(session_id);
        session_id.clear();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            session_id_.clear();
        }
        return false;
    }
    transport_.setCallbacks(createPeerCallbacks());
    transport_.setMediaEnabled(false);

    bool negotiated = false;
    {
        std::lock_guard<std::mutex> api_lock(session_api_mutex_);
        if (!isCancelled(callbacks)) {
            negotiated = negotiateWebRtc(profile, session_id, callbacks);
        }
    }
    if (!negotiated ||
        !transport_.waitDataChannels(kDataChannelTimeout, cancel)) {
        lunar::diagnosticLog(
            "xbox-stream", "Fresh WebRTC negotiation/data channel failed");
        transport_.disconnect();
        channels_.reset();
        session_client_.deleteSessionAsync(session_id);
        session_id.clear();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            session_id_.clear();
        }
        return false;
    }

    rememberSuccessfulIcePreferences(transport_, profile);

    control_recovery_requested_ = false;
    lunar::persistentEventLog(
        "xbox-stream", "fresh-reconnect phase=session-established");
    lunar::diagnosticLog("xbox-stream", "Fresh WebRTC session established id=%s",
                         session_id.c_str());
    return true;
}

webrtc::PeerCallbacks XboxStreamSession::createPeerCallbacks() {
    return {
        [this](const uint8_t* data, size_t len, uint16_t sequence, uint64_t timestamp) {
            (void)sequence;
            perf_.recordPackets(1, 0);
            perf_.recordVideoPacket(len, timestamp);
            media_.decodeVideoPacket(data, len, timestamp);
        },
        [this](size_t bytes) {
            perf_.recordVideoNetworkBytes(bytes);
        },
        [this](const uint8_t* data, size_t len, uint16_t sequence, uint64_t timestamp) {
            perf_.recordPackets(1, 0);
            perf_.recordAudioPacket();
            media_.decodeAudioPacket(data, len, sequence, timestamp);
        },
        [this](const std::string& label, const uint8_t* data, size_t len) {
            if (label == "message") {
                channels_.handleMessageChannelData(data, len);
            }
        },
        [](const std::string& error) {
            std::fprintf(stderr, "[ctrl] %s\n", error.c_str());
        },
        [this](bool reset_decoder) {
            media_.requestVideoRecovery(reset_decoder
                                            ? "RTP loss timeout"
                                            : "waiting for recovery IDR",
                                        reset_decoder);
        },
        [this](uint32_t ssrc) {
            lunar::persistentEventLog(
                "xbox-stream",
                "video-source-discontinuity ssrc=%u action=source-reset",
                ssrc);
            media_.prepareForNewVideoSource("RTP source discontinuity");
        },
        [this](uint8_t gamepad_index,
               float left,
               float right,
               float left_trigger,
               float right_trigger,
               uint16_t duration_ms,
               uint16_t delay_ms,
               uint8_t repeat) {
            rumble_.setRumble(gamepad_index,
                              left,
                              right,
                              left_trigger,
                              right_trigger,
                              duration_ms,
                              delay_ms,
                              repeat);
        },
    };
}

void XboxStreamSession::runLoop(StreamProfile profile,
                                std::string session_id,
                                int keep_alive_seconds,
                                RuntimeCallbacks callbacks) {
    lunar::diagnosticLog("xbox-stream", "runLoop begin session=%s keep_alive=%d",
                         session_id.c_str(),
                         keep_alive_seconds);
    auto last_reconnect = std::chrono::steady_clock::now();
    auto last_perf_log = std::chrono::steady_clock::now();
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
    auto last_latency_log = std::chrono::steady_clock::now();
    uint64_t latency_window_id = 0;
    auto previous_writer_stats = lunar::asyncDiagnosticWriterStats();
    uint32_t previous_rtp_video = perf_.rtp_video_packets.load();
    uint32_t previous_h264_corrupt = perf_.h264_corrupt_frames.load();
    uint32_t previous_rtp_queue_drops = perf_.rtp_queue_drops.load();
    uint32_t previous_srtp_fail = perf_.srtp_rtp_decrypt_failures.load();
    uint32_t previous_video_aus = perf_.video_packets.load();
    uint32_t previous_video_frames = perf_.video_frames.load();
    uint32_t previous_audio_frames = perf_.audio_frames.load();
    uint32_t previous_video_drops = perf_.video_frame_drops.load();
    uint32_t previous_sync_drops = perf_.video_sync_drops.load();
    uint32_t previous_decode_errors = perf_.video_decode_errors.load();
    uint32_t previous_pending_drops = perf_.decoded_pending_drop_oldest.load();
    uint32_t previous_catchup_suppressed =
        perf_.decoded_catchup_suppressed.load();
    uint32_t previous_audio_drops = perf_.audio_drops.load();
    uint64_t previous_encoded_video_bytes = perf_.encoded_video_bytes.load();
    uint64_t previous_received_video_bytes = perf_.received_video_bytes.load();
#endif
    auto last_keyframe_request = std::chrono::steady_clock::time_point{};
    auto last_receiver_feedback = std::chrono::steady_clock::time_point{};
    AdaptiveBitrateController bitrate_controller(
        profile.type == SessionType::Cloud
            ? webrtc::NetworkPathMode::Cloud
            : webrtc::NetworkPathMode::Home,
        streamProfileBitrateKbps(profile));
    XboxLatencyController latency_controller(
        profile.type == SessionType::Cloud);
    auto active_latency_state = latency_controller.state();
    VideoRecoveryRequestPolicy recovery_pli_policy;
    uint32_t last_perf_rendered = 0;
    uint32_t keyframe_missing_baseline = 0;
    uint32_t keyframe_corrupt_baseline = 0;
    uint32_t keyframe_queue_drop_baseline = 0;
    uint32_t keyframe_srtp_baseline = 0;
    uint32_t adaptation_corrupt_baseline = 0;
    bool adaptation_waiting_keyframe = false;
    int reconnect_count = 0;
    uint32_t renderer_recovery_attempts = 0;
    uint32_t decoder_recovery_attempts = 0;
    auto last_health_recovery = std::chrono::steady_clock::time_point{};
    auto presentation_resumed_at = std::chrono::steady_clock::time_point{};
    bool presentation_was_suspended = false;
    auto video_watchdog_started = std::chrono::steady_clock::time_point{};
    auto next_health_poll = std::chrono::steady_clock::now();
    stream::MediaHealthStats media_health{};
    uint32_t source_discontinuity_baseline = 0;
    int input_send_failure_logs = 0;
    bool control_started = false;
    bool first_loop_logged = false;
    bool first_input_logged = false;
    bool reconnect_prepared = false;
    std::string reconnect_reason = "connection-state";
    auto next_network_tick = std::chrono::steady_clock::now();
    auto next_rumble_tick = next_network_tick;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    auto last_webrtc_pump = std::chrono::steady_clock::time_point{};
#endif
    auto cancel = [this, callbacks]() { return isCancelled(callbacks); };
    auto request_fresh_reconnect = [&](const char* reason) {
        reconnect_reason = reason ? reason : "connection-state";
        if (!reconnect_prepared) {
            prepareFreshSessionReconnect(reconnect_reason.c_str());
            reconnect_prepared = true;
        }
    };

    try {
    while (streaming_.load() && !isCancelled(callbacks)) {
        if (!first_loop_logged) {
            lunar::diagnosticLog("xbox-stream", "runLoop first iteration connected=%s data_ready=%s",
                                 transport_.isConnected() ? "true" : "false",
                                 transport_.isDataChannelReady() ? "true" : "false");
            first_loop_logged = true;
        }
        const auto loop_started = std::chrono::steady_clock::now();
        const bool connected_before_pump = transport_.isConnected();
        if (control_started && connected_before_pump) {
            const uint64_t input_now_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    loop_started.time_since_epoch()).count());
            auto input_snapshot = input_accumulator_.peekTransition(input_now_ns);
            const bool input_is_transition = input_snapshot.has_value();
            if (!input_snapshot) {
                input_snapshot = input_accumulator_.peekLatest();
            }
            if (input_snapshot) {
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
                if (input_snapshot->sampled_at_ns > 0 &&
                    input_now_ns >= input_snapshot->sampled_at_ns) {
                    perf_.recordInputSnapshotAge(
                        (input_now_ns - input_snapshot->sampled_at_ns) / 1000ULL);
                }
#endif
                const auto input_packet = xinput_.encode(input_snapshot->state);
                if (!first_input_logged) {
                    lunar::diagnosticLog(
                        "xbox-stream",
                        "first input path connected=true packet_len=%zu",
                        input_packet.size());
                    std::fprintf(
                        stderr,
                        "[xbox-stream] first input path connected=true packet_len=%zu\n",
                        input_packet.size());
                    first_input_logged = true;
                }
                const bool input_queued = input_is_transition
                    ? channels_.sendInputTransitionPacket(
                          input_packet.data(), input_packet.size())
                    : channels_.sendInputPacket(
                          input_packet.data(), input_packet.size());
                if (input_queued) {
                    if (input_is_transition) {
                        input_accumulator_.commitTransition(*input_snapshot);
                    } else {
                        input_accumulator_.commitLatest(*input_snapshot);
                    }
                    perf_.recordInputPacket();
                } else if (input_send_failure_logs < 8) {
                    lunar::diagnosticLog("xbox-stream",
                                         "input send failed sequence_attempt=%d",
                                         input_send_failure_logs + 1);
                    input_send_failure_logs++;
                }
            }
        }
        if (loop_started >= next_rumble_tick) {
            try {
                rumble_.update();
            } catch (const std::exception& e) {
                lunar::diagnosticLog("xbox-stream",
                                     "rumble update exception: %s", e.what());
            } catch (...) {
                lunar::diagnosticLog("xbox-stream",
                                     "rumble update unknown exception");
            }
            next_rumble_tick += kRumbleUpdateInterval;
            if (next_rumble_tick <= loop_started) {
                next_rumble_tick = loop_started + kRumbleUpdateInterval;
            }
        }

#if LUNARNX_DROP_DIAGNOSTIC_LOG
        const auto webrtc_pump_started = std::chrono::steady_clock::now();
        const uint64_t webrtc_pump_gap_us =
            last_webrtc_pump.time_since_epoch().count() == 0
                ? 0
                : static_cast<uint64_t>(
                      std::chrono::duration_cast<std::chrono::microseconds>(
                          webrtc_pump_started - last_webrtc_pump).count());
        transport_.processEvents();
        const auto webrtc_pump_finished = std::chrono::steady_clock::now();
        perf_.recordWebRtcPump(
            webrtc_pump_gap_us,
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    webrtc_pump_finished - webrtc_pump_started).count()));
        last_webrtc_pump = webrtc_pump_started;
#else
        transport_.processEvents();
#endif
        if (control_recovery_requested_.exchange(false)) {
            lunar::persistentEventLog(
                "xbox-stream",
                "WebRTC reconnect requested reason=control-plane-health");
            request_fresh_reconnect("control-plane-health");
        }
        if (transport_.consumeDataChannelFailure()) {
            lunar::diagnosticLog("xbox-stream",
                                 "DataChannel send budget exhausted; reconnecting");
            lunar::persistentEventLog(
                "xbox-stream",
                "WebRTC reconnect requested reason=data-channel-send-failure");
            request_fresh_reconnect("data-channel-send-failure");
        }
        const bool connected = transport_.isConnected();
        if (!connected && input_delivery_ready_.exchange(false)) {
            input_accumulator_.prepareForReconnect();
        }
        const auto media_stats = transport_.getMediaStats();
        const auto health_poll_now = std::chrono::steady_clock::now();
        const bool health_poll_due = health_poll_now >= next_health_poll;
        if (health_poll_due) {
            media_health = media_.getHealthStats();
            next_health_poll = health_poll_now + kMediaHealthPollInterval;
        }
        const bool pipeline_recovery_pending = media_.hasVideoRecoveryRequest();
        const bool latency_recovery_pending = pipeline_recovery_pending ||
            media_stats.video_waiting_keyframe;
        const auto desired_latency_state = latency_controller.observe(
            media_stats.network_path, latency_recovery_pending);
        if (desired_latency_state.mode != active_latency_state.mode) {
            media_.setVideoPresentationMode(
                desired_latency_state.video_presentation);
            media_.setVideoDecodeCatchUpMode(
                desired_latency_state.video_decode_catch_up);
            const bool audio_changed = media_.setAudioLatencyMode(
                desired_latency_state.audio_latency);
            lunar::persistentEventLog(
                "xbox-latency",
                "mode=%s->%s presentation=%s catchup=%s audio=%s "
                "audio_changed=%d quality=%s observed=%s recovery=%d",
                xboxLatencyModeName(active_latency_state.mode),
                xboxLatencyModeName(desired_latency_state.mode),
                stream::videoPresentationModeName(
                    desired_latency_state.video_presentation),
                stream::videoDecodeCatchUpModeName(
                    desired_latency_state.video_decode_catch_up),
                stream::audioLatencyModeName(
                    desired_latency_state.audio_latency),
                audio_changed ? 1 : 0,
                webrtc::networkPathQualityName(
                    media_stats.network_path.quality),
                webrtc::networkPathQualityName(
                    media_stats.network_path.observed_quality),
                latency_recovery_pending ? 1 : 0);
            active_latency_state = desired_latency_state;
        }
        perf_.setRtpStats(media_stats.video_rtp_packets,
                          media_stats.audio_rtp_packets,
                          media_stats.video_rtp_sequence_gaps,
                          media_stats.audio_rtp_sequence_gaps,
                          media_stats.video_rtp_missing_packets,
                          media_stats.video_rtp_missing_packets_detected,
                          media_stats.audio_rtp_missing_packets,
                          media_stats.video_h264_frames,
                          media_stats.video_h264_corrupt_frames,
                          media_stats.video_h264_unsupported_nalus,
                          media_stats.video_h264_overflow_frames,
                          media_stats.video_h264_max_frame_bytes,
                          media_stats.rtp_queue_drops,
                          media_stats.rtp_queue_high_watermark,
                          media_stats.srtp_rtp_decrypt_failures,
                          media_stats.srtp_rtp_auth_failures,
                          media_stats.srtp_rtp_replay_failures,
                          media_stats.srtp_rtp_replay_old_failures,
                          media_stats.srtp_rtp_other_failures,
                          media_stats.srtp_rtcp_decrypt_failures,
                          media_stats.ice_rtt_ms,
                          media_stats.video_rtp_highest_seq_ext,
                          media_stats.video_rtp_nacks,
                          media_stats.video_rtp_resyncs,
                          media_stats.video_rtp_last_gap_packets,
                          media_stats.video_rtp_ssrc,
                          media_stats.video_rtp_ssrc_changes,
                          media_stats.video_rtp_arrival_age_ms,
                          media_stats.video_rtp_last_arrival_gap_ms,
                          media_stats.video_rtp_max_arrival_gap_ms,
                          media_stats.video_jitter_buffered_packets,
                          media_stats.video_jitter_buffered_frames,
                          media_stats.video_jitter_buffered_bytes,
                          media_stats.video_waiting_keyframe);

        if (control_started && connected &&
            (last_receiver_feedback.time_since_epoch().count() == 0 ||
             std::chrono::steady_clock::now() - last_receiver_feedback >=
                 kReceiverFeedbackInterval)) {
            const int previous_bitrate_kbps = bitrate_controller.targetKbps();
            AdaptiveBitrateSignal bitrate_signal;
            bitrate_signal.requested_width = profile.width;
            bitrate_signal.requested_height = profile.height;
            bitrate_signal.decoded_width = static_cast<int>(
                perf_.decoded_video_width.load());
            bitrate_signal.decoded_height = static_cast<int>(
                perf_.decoded_video_height.load());
            bitrate_signal.hard_recovery =
                media_stats.video_h264_corrupt_frames >
                    adaptation_corrupt_baseline ||
                (media_stats.video_waiting_keyframe &&
                 !adaptation_waiting_keyframe);
            const int target_bitrate_kbps =
                bitrate_controller.observe(media_stats.network_path,
                                            bitrate_signal);
            adaptation_corrupt_baseline =
                media_stats.video_h264_corrupt_frames;
            adaptation_waiting_keyframe =
                media_stats.video_waiting_keyframe;
            if (target_bitrate_kbps != previous_bitrate_kbps) {
                lunar::persistentEventLog(
                    "xbox-bitrate",
                    "adaptive REMB bitrate_kbps=%d->%d quality=%s "
                    "detected=%u recovered=%u unrecovered=%u "
                    "unrecovered_loss_ppm=%llu received_kbps=%u "
                    "queue_depth=%u queue_drops=%u rtt_ms=%u "
                    "baseline_ms=%u inflation_ms=%u",
                    previous_bitrate_kbps,
                    target_bitrate_kbps,
                    webrtc::networkPathQualityName(
                        media_stats.network_path.quality),
                    media_stats.network_path.detected_missing,
                    media_stats.network_path.recovered_missing,
                    media_stats.network_path.unrecovered_missing,
                    static_cast<unsigned long long>(
                        media_stats.network_path.unrecovered_loss_ppm),
                    media_stats.network_path.received_bitrate_kbps,
                    media_stats.network_path.queue_depth,
                    media_stats.network_path.queue_drops,
                    media_stats.network_path.raw_rtt_ms,
                    media_stats.network_path.baseline_rtt_ms,
                    media_stats.network_path.rtt_inflation_ms);
            }
            const uint32_t bitrate_bps = static_cast<uint32_t>(
                target_bitrate_kbps) * 1000u;
            transport_.sendReceiverFeedback(bitrate_bps);
            last_receiver_feedback = std::chrono::steady_clock::now();
        }
        if (!control_started && transport_.isDataChannelReady()) {
            // Match XStreaming Control.start() sequence which already completed inside
            // startProtocol (auth -> gamepadRemoved -> delay -> gamepadAdded -> metadata).
            bool ok = false;
            try {
                lunar::diagnosticLog("xbox-stream", "control protocol start begin");
                auto metadata = xinput_.encodeMetadata(0);
                ok = channels_.startProtocol(profile, metadata.data(), metadata.size(), cancel);
                lunar::diagnosticLog("xbox-stream",
                                     "control protocol start result=%s metadata_len=%zu",
                                     ok ? "true" : "false", metadata.size());
                std::fprintf(stderr, "[xbox-stream] control start result=%s\n",
                             ok ? "true" : "false");
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[xbox-stream] control start exception: %s\n", e.what());
                lunar::diagnosticLog("xbox-stream", "control start exception: %s", e.what());
                ok = false;
            } catch (...) {
                std::fprintf(stderr, "[xbox-stream] control start unknown exception\n");
                lunar::diagnosticLog("xbox-stream", "control start unknown exception");
                ok = false;
            }

            if (ok) {
                video_watchdog_started = std::chrono::steady_clock::now();
                next_health_poll = video_watchdog_started;
                input_accumulator_.prepareForReconnect();
                try {
                    // Enable RTP handling and request keyframe once after auth.
                    transport_.setMediaEnabled(true);
                    std::fprintf(stderr, "[xbox-stream] media enabled\n");
                    const bool keyframe_requested =
                        channels_.requestVideoKeyframe(false);
                    perf_.recordRecoveryPli(keyframe_requested);
                    std::fprintf(stderr, "[xbox-stream] initial keyframe=%s\n",
                                 keyframe_requested ? "true" : "false");
                    lunar::diagnosticLog("xbox-stream", "initial keyframe request result=%s",
                                         keyframe_requested ? "true" : "false");
                    last_keyframe_request = std::chrono::steady_clock::now();
                    recovery_pli_policy.reset();
                    keyframe_missing_baseline =
                        media_stats.video_rtp_missing_packets_detected;
                    keyframe_corrupt_baseline = media_stats.video_h264_corrupt_frames;
                    keyframe_queue_drop_baseline = media_stats.rtp_queue_drops;
                    keyframe_srtp_baseline = media_stats.srtp_rtp_decrypt_failures;
                    control_started = true;
                    input_delivery_ready_ = true;
                    media_startup_ready_ = true;
                    lunar::persistentEventLog(
                        "xbox-startup",
                        "phase=media-enabled video_rtp=%u audio_rtp=%u "
                        "rtp_queue_high=%u rtp_queue_drops=%u",
                        media_stats.video_rtp_packets,
                        media_stats.audio_rtp_packets,
                        media_stats.rtp_queue_high_watermark,
                        media_stats.rtp_queue_drops);
                } catch (const std::exception& e) {
                    lunar::dropDiagnosticLog(
                        "xbox-stream", "post-control media exception: %s", e.what());
                } catch (...) {
                    lunar::dropDiagnosticLog(
                        "xbox-stream", "post-control media unknown exception");
                }
            }
            if (!control_started && isCancelled(callbacks)) {
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (media_health.presentation_suspended) {
            presentation_was_suspended = true;
            renderer_recovery_attempts = 0;
            last_health_recovery = {};
        } else if (presentation_was_suspended) {
            presentation_was_suspended = false;
            presentation_resumed_at = now;
            renderer_recovery_attempts = 0;
            last_health_recovery = {};
        }
        const bool presentation_resume_grace =
            presentation_resumed_at.time_since_epoch().count() != 0 &&
            now - presentation_resumed_at < kVideoPresentStallTimeout;
        const uint32_t source_discontinuities =
            media_stats.video_rtp_ssrc_changes +
            media_stats.video_rtp_timestamp_discontinuities;
        if (source_discontinuities > source_discontinuity_baseline) {
            source_discontinuity_baseline = source_discontinuities;
            video_watchdog_started = now;
            renderer_recovery_attempts = 0;
            decoder_recovery_attempts = 0;
            last_health_recovery = {};
        }
        if (control_started && connected) {
            const uint32_t missing_delta =
                media_stats.video_rtp_missing_packets_detected -
                keyframe_missing_baseline;
            const uint32_t corrupt_delta =
                media_stats.video_h264_corrupt_frames - keyframe_corrupt_baseline;
            const uint32_t queue_drop_delta =
                media_stats.rtp_queue_drops - keyframe_queue_drop_baseline;
            const uint32_t srtp_delta =
                media_stats.srtp_rtp_decrypt_failures - keyframe_srtp_baseline;
            const bool video_damage_increased =
                missing_delta >= kRecoveryMissingPacketsThreshold ||
                corrupt_delta >= kRecoveryCorruptFramesThreshold ||
                queue_drop_delta > 0;
            const uint32_t rendered_frames = perf_.video_frames.load();
            const bool awaiting_first_frame = rendered_frames == 0;
            const bool can_retry_startup_keyframe =
                awaiting_first_frame &&
                (last_keyframe_request.time_since_epoch().count() == 0 ||
                 now - last_keyframe_request >= kStartupKeyframeRetryInterval);
            const bool recovery_active = video_damage_increased ||
                pipeline_recovery_pending ||
                media_stats.video_waiting_keyframe;
            const uint64_t recovery_now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count());
            const bool can_request_recovery_keyframe = !awaiting_first_frame &&
                recovery_pli_policy.shouldRequest(recovery_active,
                                                  recovery_now_ms);
            if (can_retry_startup_keyframe) {
                const bool keyframe_requested =
                    channels_.requestVideoKeyframe(false);
                perf_.recordRecoveryPli(keyframe_requested);
                lunar::diagnosticLog("xbox-stream",
                                     "startup keyframe retry result=%s rendered=%u missing=%u(+%u) corrupt=%u(+%u) srtp=%u(+%u)",
                                     keyframe_requested ? "true" : "false",
                                     rendered_frames,
                                     media_stats.video_rtp_missing_packets,
                                     missing_delta,
                                     media_stats.video_h264_corrupt_frames,
                                     corrupt_delta,
                                     media_stats.srtp_rtp_decrypt_failures,
                                     srtp_delta);
                if (keyframe_requested) {
                    media_.clearVideoRecoveryRequest();
                }
                last_keyframe_request = now;
                keyframe_missing_baseline =
                    media_stats.video_rtp_missing_packets_detected;
                keyframe_corrupt_baseline = media_stats.video_h264_corrupt_frames;
                keyframe_queue_drop_baseline = media_stats.rtp_queue_drops;
                keyframe_srtp_baseline = media_stats.srtp_rtp_decrypt_failures;
            } else if (can_request_recovery_keyframe) {
                const bool keyframe_requested =
                    channels_.requestVideoKeyframe(false);
                recovery_pli_policy.recordAttempt(recovery_now_ms);
                perf_.recordRecoveryPli(keyframe_requested);
                lunar::diagnosticLog("xbox-stream",
                                     "recovery keyframe request result=%s attempt=%u pipeline=%s missing=%u(+%u) corrupt=%u(+%u) queue_drop=%u(+%u) srtp=%u(+%u)",
                                     keyframe_requested ? "true" : "false",
                                     recovery_pli_policy.attempts(),
                                     pipeline_recovery_pending ? "true" : "false",
                                     media_stats.video_rtp_missing_packets,
                                     missing_delta,
                                     media_stats.video_h264_corrupt_frames,
                                     corrupt_delta,
                                     media_stats.rtp_queue_drops,
                                     queue_drop_delta,
                                     media_stats.srtp_rtp_decrypt_failures,
                                     srtp_delta);
                if (keyframe_requested) {
                    media_.clearVideoRecoveryRequest();
                }
                keyframe_missing_baseline =
                    media_stats.video_rtp_missing_packets_detected;
                keyframe_corrupt_baseline = media_stats.video_h264_corrupt_frames;
                keyframe_queue_drop_baseline = media_stats.rtp_queue_drops;
                keyframe_srtp_baseline = media_stats.srtp_rtp_decrypt_failures;
            }

            const bool rtp_seen = media_stats.video_rtp_packets > 0;
            const bool watchdog_started =
                video_watchdog_started.time_since_epoch().count() != 0;
            const auto video_watchdog_age = watchdog_started
                ? now - video_watchdog_started
                : std::chrono::steady_clock::duration::zero();
            const bool rtp_stalled =
                (rtp_seen && media_stats.video_rtp_arrival_age_ms >=
                                  static_cast<uint32_t>(kVideoRtpStallTimeout.count())) ||
                (!rtp_seen && watchdog_started && video_watchdog_age >=
                                  kVideoRtpStallTimeout);
            bool decode_stalled = false;
            bool present_stalled = false;
            if (health_poll_due) {
                const bool rtp_alive = rtp_seen &&
                    media_stats.video_rtp_arrival_age_ms <
                        static_cast<uint32_t>(kVideoRtpStallTimeout.count());
                decode_stalled = rtp_alive && watchdog_started &&
                    ((!media_health.has_decoded_video &&
                      video_watchdog_age >= kVideoDecodeStallTimeout) ||
                     (media_health.has_decoded_video &&
                      media_health.decoded_video_age_ms >=
                          static_cast<uint64_t>(kVideoDecodeStallTimeout.count())));
                present_stalled = media_health.has_decoded_video &&
                    ((media_health.has_presented_video &&
                      media_health.presented_video_age_ms >=
                          static_cast<uint64_t>(kVideoPresentStallTimeout.count())) ||
                     (!media_health.has_presented_video &&
                      media_health.decoded_video_age_ms >=
                          static_cast<uint64_t>(kVideoPresentStallTimeout.count())) ||
                     media_health.consecutive_render_faults >= 2);
            }

            const bool recovery_due =
                last_health_recovery.time_since_epoch().count() == 0 ||
                now - last_health_recovery >= kVideoHealthRecoveryCooldown;
            const VideoWatchdogObservation watchdog_observation{
                rtp_stalled,
                decode_stalled,
                present_stalled,
                media_health.presentation_suspended ||
                    presentation_resume_grace,
                recovery_due,
                media_.hasRendererRecoveryPending(),
                renderer_recovery_attempts,
                decoder_recovery_attempts,
            };
            const auto watchdog_action =
                decideVideoWatchdogAction(watchdog_observation);

            if (watchdog_action == VideoWatchdogAction::ReconnectSession) {
                const char* reason = rtp_stalled
                    ? "video-rtp-liveness-timeout"
                    : "video-decode-recovery-exhausted";
                lunar::persistentEventLog(
                    "xbox-stream",
                    "video watchdog decode_age_ms=%llu present_age_ms=%llu "
                    "rtp_age_ms=%u faults=%u render_stage=%s "
                    "render_stage_age_ms=%llu action=fresh-session-reconnect "
                    "reason=%s",
                    static_cast<unsigned long long>(media_health.decoded_video_age_ms),
                    static_cast<unsigned long long>(media_health.presented_video_age_ms),
                    media_stats.video_rtp_arrival_age_ms,
                    media_health.render_fault_count,
                    videoRenderStageName(media_health.renderer_stage),
                    static_cast<unsigned long long>(
                        media_health.renderer_stage_age_ms),
                    reason);
                request_fresh_reconnect(reason);
            } else if (watchdog_action ==
                       VideoWatchdogAction::RecoverDecoder) {
                lunar::persistentEventLog(
                    "xbox-stream",
                    "video watchdog decode_age_ms=%llu present_age_ms=%llu "
                    "rtp_age_ms=%u faults=%u render_stage=%s "
                    "render_stage_age_ms=%llu action=decoder-recovery",
                    static_cast<unsigned long long>(media_health.decoded_video_age_ms),
                    static_cast<unsigned long long>(media_health.presented_video_age_ms),
                    media_stats.video_rtp_arrival_age_ms,
                    media_health.render_fault_count,
                    videoRenderStageName(media_health.renderer_stage),
                    static_cast<unsigned long long>(
                        media_health.renderer_stage_age_ms));
                media_.requestVideoRecovery("video decode watchdog", true);
                ++decoder_recovery_attempts;
                last_health_recovery = now;
            } else if (watchdog_action ==
                       VideoWatchdogAction::RecoverRenderer) {
                lunar::persistentEventLog(
                    "xbox-stream",
                    "video watchdog decode_age_ms=%llu present_age_ms=%llu "
                    "rtp_age_ms=%u faults=%u render_stage=%s "
                    "render_stage_age_ms=%llu action=renderer-recovery",
                    static_cast<unsigned long long>(media_health.decoded_video_age_ms),
                    static_cast<unsigned long long>(media_health.presented_video_age_ms),
                    media_stats.video_rtp_arrival_age_ms,
                    media_health.render_fault_count,
                    videoRenderStageName(media_health.renderer_stage),
                    static_cast<unsigned long long>(
                        media_health.renderer_stage_age_ms));
                media_.requestRendererRecovery("video present watchdog");
                ++renderer_recovery_attempts;
                last_health_recovery = now;
            } else if (watchdog_action ==
                       VideoWatchdogAction::ObserveRendererRecovery) {
                lunar::persistentEventLog(
                    "xbox-stream",
                    "video watchdog decode_age_ms=%llu present_age_ms=%llu "
                    "rtp_age_ms=%u faults=%u render_stage=%s "
                    "render_stage_age_ms=%llu "
                    "action=renderer-recovery-grace",
                    static_cast<unsigned long long>(
                        media_health.decoded_video_age_ms),
                    static_cast<unsigned long long>(
                        media_health.presented_video_age_ms),
                    media_stats.video_rtp_arrival_age_ms,
                    media_health.render_fault_count,
                    videoRenderStageName(media_health.renderer_stage),
                    static_cast<unsigned long long>(
                        media_health.renderer_stage_age_ms));
                ++renderer_recovery_attempts;
                last_health_recovery = now;
            } else if (watchdog_action == VideoWatchdogAction::StopStream) {
                lunar::persistentEventLog(
                    "xbox-stream",
                    "video watchdog decode_age_ms=%llu present_age_ms=%llu "
                    "rtp_age_ms=%u faults=%u render_stage=%s "
                    "render_stage_age_ms=%llu action=stream-error "
                    "reason=renderer-recovery-exhausted",
                    static_cast<unsigned long long>(media_health.decoded_video_age_ms),
                    static_cast<unsigned long long>(media_health.presented_video_age_ms),
                    media_stats.video_rtp_arrival_age_ms,
                    media_health.render_fault_count,
                    videoRenderStageName(media_health.renderer_stage),
                    static_cast<unsigned long long>(
                        media_health.renderer_stage_age_ms));
                streaming_ = false;
                notify(callbacks.on_error,
                       "Video presentation stalled. Please restart the stream.");
                break;
            }

            if (health_poll_due && !decode_stalled && !present_stalled &&
                media_health.has_presented_video &&
                media_health.presented_video_age_ms < 1000 &&
                media_health.consecutive_render_faults == 0) {
                renderer_recovery_attempts = 0;
                decoder_recovery_attempts = 0;
                last_health_recovery = {};
            }
        }

#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        if (now - last_latency_log >= std::chrono::seconds(1)) {
            const uint64_t window_ms = static_cast<uint64_t>(std::max<int64_t>(
                1, std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_latency_log).count()));
            last_latency_log = now;
            ++latency_window_id;

            const auto perf_window = perf_.takeLatencyWindow();
            const auto peer_window = transport_.takeLatencyWindow();
            const auto writer_stats = lunar::asyncDiagnosticWriterStats();
            const auto delta64 = [](uint64_t current, uint64_t previous) {
                return current >= previous ? current - previous : current;
            };
            const auto delta32 = [](uint32_t current, uint32_t previous) {
                return current >= previous ? current - previous : current;
            };

            const uint32_t rtp_video = perf_.rtp_video_packets.load();
            const uint32_t h264_corrupt = perf_.h264_corrupt_frames.load();
            const uint32_t rtp_queue_drops = perf_.rtp_queue_drops.load();
            const uint32_t srtp_fail = perf_.srtp_rtp_decrypt_failures.load();
            const uint32_t video_aus = perf_.video_packets.load();
            const uint32_t video_frames = perf_.video_frames.load();
            const uint32_t audio_frames = perf_.audio_frames.load();
            const uint32_t video_drops = perf_.video_frame_drops.load();
            const uint32_t sync_drops = perf_.video_sync_drops.load();
            const uint32_t decode_errors = perf_.video_decode_errors.load();
            const uint32_t pending_drops =
                perf_.decoded_pending_drop_oldest.load();
            const uint32_t catchup_suppressed =
                perf_.decoded_catchup_suppressed.load();
            const uint32_t audio_drops = perf_.audio_drops.load();
            const uint64_t encoded_video_bytes =
                perf_.encoded_video_bytes.load();
            const uint64_t received_video_bytes =
                perf_.received_video_bytes.load();
            const uint64_t encoded_kbps =
                delta64(encoded_video_bytes, previous_encoded_video_bytes) *
                8ULL / window_ms;
            const uint64_t payload_kbps =
                delta64(received_video_bytes, previous_received_video_bytes) *
                8ULL / window_ms;
            const float display_fps =
                static_cast<float>(perf_window.presented_new_frames) *
                1000.0f / static_cast<float>(window_ms);
            const auto jitter_policy = webrtc::computeVideoJitterPolicy(
                profile.type == SessionType::Cloud
                    ? webrtc::VideoJitterMode::Cloud
                    : webrtc::VideoJitterMode::Home,
                media_stats.network_path);

#if LUNARNX_DROP_DIAGNOSTIC_LOG
            const uint32_t encoded_queue_packets =
                perf_.video_queue_packets.load();
            const uint64_t encoded_queue_bytes = perf_.video_queue_bytes.load();
            const uint32_t encoded_queue_oldest_ms =
                perf_.video_queue_oldest_age_ms.load();
            const int64_t av_raw_us = perf_.last_av_raw_delay_ns.load() / 1000;
            const int64_t av_policy_us =
                perf_.last_av_policy_delay_ns.load() / 1000;
            const int64_t av_audio_age_ms = perf_.last_av_audio_age_ms.load();
            const bool av_audio_master =
                perf_.last_av_using_audio_master.load();
#else
            const uint32_t encoded_queue_packets = 0;
            const uint64_t encoded_queue_bytes = 0;
            const uint32_t encoded_queue_oldest_ms = 0;
            const int64_t av_raw_us = 0;
            const int64_t av_policy_us = 0;
            const int64_t av_audio_age_ms = -1;
            const bool av_audio_master = false;
#endif

            lunar::latencyDiagnosticLog(
                "network",
                "window=%llu elapsed_ms=%llu connected=%d data_ready=%d "
                "control=%d mode=%s path_valid=%d quality=%s observed=%s "
                "ice_selected=%d ice_local=%s ice_remote=%s udp_rcvbuf=%u "
                "rtt_raw_ms=%u rtt_smoothed_ms=%u rtt_baseline_ms=%u "
                "rtt_inflation_ms=%u target_kbps=%d path_kbps=%u "
                "payload_kbps=%llu encoded_kbps=%llu "
                "path_packets=%u loss_detected=%u loss_recovered=%u "
                "loss_unrecovered=%u loss_ppm=%llu rtp_packets_delta=%u "
                "rtp_arrival_age_ms=%u rtp_arrival_gap_ms=%u "
                "rtp_queue_depth=%u rtp_queue_oldest_ms=%u "
                "rtp_queue_drops_delta=%u srtp_fail_delta=%u "
                "jitter_packets=%u jitter_frames=%u jitter_bytes=%u "
                "jitter_assembly_us=%llu/%llu/%u hold_ms=%llu "
                "missing_hold_ms=%llu recovery_hold_ms=%llu "
                "waiting_keyframe=%d",
                static_cast<unsigned long long>(latency_window_id),
                static_cast<unsigned long long>(window_ms),
                connected ? 1 : 0,
                transport_.isDataChannelReady() ? 1 : 0,
                control_started ? 1 : 0,
                profile.type == SessionType::Cloud ? "cloud" : "home",
                media_stats.network_path.valid ? 1 : 0,
                webrtc::networkPathQualityName(media_stats.network_path.quality),
                webrtc::networkPathQualityName(
                    media_stats.network_path.observed_quality),
                media_stats.ice_pair_selected ? 1 : 0,
                media_stats.ice_pair_selected
                    ? iceCandidateTypeName(media_stats.ice_local_candidate_type)
                    : "none",
                media_stats.ice_pair_selected
                    ? iceCandidateTypeName(media_stats.ice_remote_candidate_type)
                    : "none",
                media_stats.udp_receive_buffer_bytes,
                media_stats.network_path.raw_rtt_ms,
                media_stats.network_path.smoothed_rtt_ms,
                media_stats.network_path.baseline_rtt_ms,
                media_stats.network_path.rtt_inflation_ms,
                bitrate_controller.targetKbps(),
                media_stats.network_path.received_bitrate_kbps,
                static_cast<unsigned long long>(payload_kbps),
                static_cast<unsigned long long>(encoded_kbps),
                media_stats.network_path.window_packets,
                media_stats.network_path.detected_missing,
                media_stats.network_path.recovered_missing,
                media_stats.network_path.unrecovered_missing,
                static_cast<unsigned long long>(
                    media_stats.network_path.unrecovered_loss_ppm),
                delta32(rtp_video, previous_rtp_video),
                media_stats.video_rtp_arrival_age_ms,
                media_stats.video_rtp_last_arrival_gap_ms,
                media_stats.rtp_queue_depth,
                media_stats.rtp_queue_oldest_age_ms,
                delta32(rtp_queue_drops, previous_rtp_queue_drops),
                delta32(srtp_fail, previous_srtp_fail),
                media_stats.video_jitter_buffered_packets,
                media_stats.video_jitter_buffered_frames,
                media_stats.video_jitter_buffered_bytes,
                static_cast<unsigned long long>(latencyAverageUs(
                    peer_window.video_jitter.assembly_total_us,
                    peer_window.video_jitter.assembly_samples)),
                static_cast<unsigned long long>(
                    peer_window.video_jitter.assembly_max_us),
                peer_window.video_jitter.assembly_samples,
                static_cast<unsigned long long>(jitter_policy.frame_hold_ms),
                static_cast<unsigned long long>(
                    jitter_policy.missing_packet_hold_ms),
                static_cast<unsigned long long>(jitter_policy.recovery_hold_ms),
                media_stats.video_waiting_keyframe ? 1 : 0);

            lunar::latencyDiagnosticLog(
                "video-audio",
                "window=%llu elapsed_ms=%llu display_fps=%.1f "
                "displayed_new=%u au_delta=%u renderer_accept_delta=%u "
                "audio_frames_delta=%u au_queue_us=%llu/%llu/%u "
                "decode_us=%llu/%llu/%u renderer_enqueue_us=%llu/%llu/%u "
                "render_submit_us=%llu/%llu/%u "
                "render_queue_us=%llu/%llu/%u present_call_us=%llu/%llu/%u "
                "present_fence_us=%llu/%llu/%u frame_gap_us=%llu/%llu/%u "
                "encoded_queue=%u/%llu/%u video_drop_delta=%u "
                "sync_drop_delta=%u corrupt_delta=%u decode_error_delta=%u "
                "pending_drop_delta=%u catchup_suppress_delta=%u "
                "pending_depth_high=%u "
                "audio_latency_ms=%u audio_buffer_ms=%u audio_queued=%u "
                "audio_drop_delta=%u av_raw_us=%lld av_policy_us=%lld "
                "av_audio_age_ms=%lld av_master=%s decode_age_ms=%llu "
                "present_age_ms=%llu render_stage=%s stage_age_ms=%llu",
                static_cast<unsigned long long>(latency_window_id),
                static_cast<unsigned long long>(window_ms),
                display_fps,
                perf_window.presented_new_frames,
                delta32(video_aus, previous_video_aus),
                delta32(video_frames, previous_video_frames),
                delta32(audio_frames, previous_audio_frames),
                static_cast<unsigned long long>(latencyAverageUs(
                    perf_window.access_unit_queue_total_us,
                    perf_window.access_unit_queue_samples)),
                static_cast<unsigned long long>(
                    perf_window.access_unit_queue_max_us),
                perf_window.access_unit_queue_samples,
                static_cast<unsigned long long>(latencyAverageUs(
                    perf_window.decode_total_us, perf_window.decode_samples)),
                static_cast<unsigned long long>(perf_window.decode_max_us),
                perf_window.decode_samples,
                static_cast<unsigned long long>(latencyAverageUs(
                    perf_window.renderer_enqueue_total_us,
                    perf_window.renderer_enqueue_samples)),
                static_cast<unsigned long long>(
                    perf_window.renderer_enqueue_max_us),
                perf_window.renderer_enqueue_samples,
                static_cast<unsigned long long>(latencyAverageUs(
                    perf_window.render_handoff_total_us,
                    perf_window.render_handoff_samples)),
                static_cast<unsigned long long>(
                    perf_window.render_handoff_max_us),
                perf_window.render_handoff_samples,
                static_cast<unsigned long long>(latencyAverageUs(
                    perf_window.render_queue_total_us,
                    perf_window.render_queue_samples)),
                static_cast<unsigned long long>(
                    perf_window.render_queue_max_us),
                perf_window.render_queue_samples,
                static_cast<unsigned long long>(latencyAverageUs(
                    perf_window.present_call_total_us,
                    perf_window.present_call_samples)),
                static_cast<unsigned long long>(perf_window.present_call_max_us),
                perf_window.present_call_samples,
                static_cast<unsigned long long>(latencyAverageUs(
                    perf_window.present_fence_total_us,
                    perf_window.present_fence_samples)),
                static_cast<unsigned long long>(
                    perf_window.present_fence_max_us),
                perf_window.present_fence_samples,
                static_cast<unsigned long long>(latencyAverageUs(
                    perf_window.frame_submit_gap_total_us,
                    perf_window.frame_submit_gap_samples)),
                static_cast<unsigned long long>(
                    perf_window.frame_submit_gap_max_us),
                perf_window.frame_submit_gap_samples,
                encoded_queue_packets,
                static_cast<unsigned long long>(encoded_queue_bytes),
                encoded_queue_oldest_ms,
                delta32(video_drops, previous_video_drops),
                delta32(sync_drops, previous_sync_drops),
                delta32(h264_corrupt, previous_h264_corrupt),
                delta32(decode_errors, previous_decode_errors),
                delta32(pending_drops, previous_pending_drops),
                delta32(catchup_suppressed, previous_catchup_suppressed),
                perf_.decoded_pending_depth_high.load(),
                perf_.audio_latency_ms.load(),
                perf_.audio_buffer_ms.load(),
                perf_.audio_queued_buffers.load(),
                delta32(audio_drops, previous_audio_drops),
                static_cast<long long>(av_raw_us),
                static_cast<long long>(av_policy_us),
                static_cast<long long>(av_audio_age_ms),
                av_audio_master ? "audio" : "wall",
                static_cast<unsigned long long>(media_health.decoded_video_age_ms),
                static_cast<unsigned long long>(
                    media_health.presented_video_age_ms),
                videoRenderStageName(media_health.renderer_stage),
                static_cast<unsigned long long>(
                    media_health.renderer_stage_age_ms));

            lunar::latencyDiagnosticLog(
                "input-pump",
                "window=%llu elapsed_ms=%llu input_sample_gap_us=%llu/%llu/%u "
                "input_read_us=%llu/%llu/%u snapshot_age_us=%llu/%llu/%u "
                "input_enqueued=%u input_replaced=%u input_sent=%u "
                "input_fail=%u input_queue_us=%llu/%llu/%u "
                "input_send_us=%llu/%llu/%u outbound_depth=%u "
                "outbound_high=%u pump_gap_us=%llu/%llu/%u "
                "pump_total_us=%llu/%llu/%u peer_loop_us=%llu/%llu "
                "socket_us=%llu/%llu receive_us=%llu/%llu "
                "rtp_drain_us=%llu/%llu outbound_us=%llu/%llu "
                "other_us=%llu/%llu socket_packets=%llu rtp_decoded=%llu",
                static_cast<unsigned long long>(latency_window_id),
                static_cast<unsigned long long>(window_ms),
                static_cast<unsigned long long>(latencyAverageUs(
                    perf_window.input_sample_gap_total_us,
                    perf_window.input_sample_gap_samples)),
                static_cast<unsigned long long>(
                    perf_window.input_sample_gap_max_us),
                perf_window.input_sample_gap_samples,
                static_cast<unsigned long long>(latencyAverageUs(
                    perf_window.input_read_total_us,
                    perf_window.input_read_samples)),
                static_cast<unsigned long long>(perf_window.input_read_max_us),
                perf_window.input_read_samples,
                static_cast<unsigned long long>(latencyAverageUs(
                    perf_window.input_snapshot_age_total_us,
                    perf_window.input_snapshot_age_samples)),
                static_cast<unsigned long long>(
                    perf_window.input_snapshot_age_max_us),
                perf_window.input_snapshot_age_samples,
                peer_window.input_enqueued,
                peer_window.input_replaced,
                peer_window.input_sent,
                peer_window.input_send_failures,
                static_cast<unsigned long long>(latencyAverageUs(
                    peer_window.input_queue_total_us,
                    peer_window.input_queue_samples)),
                static_cast<unsigned long long>(peer_window.input_queue_max_us),
                peer_window.input_queue_samples,
                static_cast<unsigned long long>(latencyAverageUs(
                    peer_window.input_send_total_us,
                    peer_window.input_send_samples)),
                static_cast<unsigned long long>(peer_window.input_send_max_us),
                peer_window.input_send_samples,
                peer_window.outbound_queue_depth,
                peer_window.outbound_queue_high_watermark,
                static_cast<unsigned long long>(latencyAverageUs(
                    peer_window.pump_gap_total_us,
                    peer_window.pump_gap_samples)),
                static_cast<unsigned long long>(peer_window.pump_gap_max_us),
                peer_window.pump_gap_samples,
                static_cast<unsigned long long>(latencyAverageUs(
                    peer_window.pump_total_us, peer_window.pump_samples)),
                static_cast<unsigned long long>(peer_window.pump_max_us),
                peer_window.pump_samples,
                static_cast<unsigned long long>(latencyAverageUs(
                    peer_window.peer_loop_total_us,
                    peer_window.pump_samples)),
                static_cast<unsigned long long>(peer_window.peer_loop_max_us),
                static_cast<unsigned long long>(latencyAverageUs(
                    peer_window.socket_total_us, peer_window.pump_samples)),
                static_cast<unsigned long long>(peer_window.socket_max_us),
                static_cast<unsigned long long>(latencyAverageUs(
                    peer_window.receive_loop_total_us,
                    peer_window.pump_samples)),
                static_cast<unsigned long long>(peer_window.receive_loop_max_us),
                static_cast<unsigned long long>(latencyAverageUs(
                    peer_window.rtp_drain_total_us,
                    peer_window.pump_samples)),
                static_cast<unsigned long long>(peer_window.rtp_drain_max_us),
                static_cast<unsigned long long>(latencyAverageUs(
                    peer_window.outbound_total_us,
                    peer_window.pump_samples)),
                static_cast<unsigned long long>(peer_window.outbound_max_us),
                static_cast<unsigned long long>(latencyAverageUs(
                    peer_window.other_total_us, peer_window.pump_samples)),
                static_cast<unsigned long long>(peer_window.other_max_us),
                static_cast<unsigned long long>(peer_window.socket_packets),
                static_cast<unsigned long long>(
                    peer_window.rtp_packets_decoded));

            const uint64_t writer_batches = delta64(
                writer_stats.batches, previous_writer_stats.batches);
            lunar::latencyDiagnosticLog(
                "logger",
                "window=%llu elapsed_ms=%llu enqueued_delta=%llu "
                "dropped_delta=%llu queue_depth=%u queue_high=%u "
                "batches_delta=%llu bytes_delta=%llu write_us_avg=%llu "
                "write_us_total=%llu write_us_max_session=%llu "
                "opens_delta=%llu flushes_delta=%llu "
                "measurement=async-summary",
                static_cast<unsigned long long>(latency_window_id),
                static_cast<unsigned long long>(window_ms),
                static_cast<unsigned long long>(delta64(
                    writer_stats.enqueued, previous_writer_stats.enqueued)),
                static_cast<unsigned long long>(delta64(
                    writer_stats.dropped, previous_writer_stats.dropped)),
                writer_stats.queue_depth,
                writer_stats.queue_high_watermark,
                static_cast<unsigned long long>(writer_batches),
                static_cast<unsigned long long>(delta64(
                    writer_stats.bytes_written,
                    previous_writer_stats.bytes_written)),
                static_cast<unsigned long long>(latencyAverageUs(
                    delta64(writer_stats.write_total_us,
                            previous_writer_stats.write_total_us),
                    static_cast<uint32_t>(std::min<uint64_t>(
                        UINT32_MAX, writer_batches)))),
                static_cast<unsigned long long>(delta64(
                    writer_stats.write_total_us,
                    previous_writer_stats.write_total_us)),
                static_cast<unsigned long long>(writer_stats.write_max_us),
                static_cast<unsigned long long>(delta64(
                    writer_stats.file_opens,
                    previous_writer_stats.file_opens)),
                static_cast<unsigned long long>(delta64(
                    writer_stats.flushes,
                    previous_writer_stats.flushes)));

            previous_writer_stats = writer_stats;
            previous_rtp_video = rtp_video;
            previous_h264_corrupt = h264_corrupt;
            previous_rtp_queue_drops = rtp_queue_drops;
            previous_srtp_fail = srtp_fail;
            previous_video_aus = video_aus;
            previous_video_frames = video_frames;
            previous_audio_frames = audio_frames;
            previous_video_drops = video_drops;
            previous_sync_drops = sync_drops;
            previous_decode_errors = decode_errors;
            previous_pending_drops = pending_drops;
            previous_catchup_suppressed = catchup_suppressed;
            previous_audio_drops = audio_drops;
            previous_encoded_video_bytes = encoded_video_bytes;
            previous_received_video_bytes = received_video_bytes;
        }
#endif

        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_perf_log).count() >= 5) {
            const uint32_t rtp_video = perf_.rtp_video_packets.load();
            const uint32_t rtp_audio = perf_.rtp_audio_packets.load();
            const uint32_t video_gaps = perf_.rtp_video_sequence_gaps.load();
            const uint32_t audio_gaps = perf_.rtp_audio_sequence_gaps.load();
            const uint32_t video_missing = perf_.rtp_video_missing_packets.load();
            const uint32_t audio_missing = perf_.rtp_audio_missing_packets.load();
            const uint32_t h264_ok = perf_.h264_frames.load();
            const uint32_t h264_corrupt = perf_.h264_corrupt_frames.load();
            const uint32_t h264_unsupported = perf_.h264_unsupported_nalus.load();
            const uint32_t h264_overflow = perf_.h264_overflow_frames.load();
            const uint32_t queue_drops = perf_.rtp_queue_drops.load();
            const uint32_t queue_high = perf_.rtp_queue_high_watermark.load();
            const uint32_t srtp_rtp_fail = perf_.srtp_rtp_decrypt_failures.load();
            const uint32_t srtp_rtcp_fail = perf_.srtp_rtcp_decrypt_failures.load();
            const uint32_t video_aus = perf_.video_packets.load();
            const uint32_t audio_aus = perf_.audio_packets.load();
            const uint32_t rendered = perf_.video_frames.load();
            const uint32_t decode_errors = perf_.video_decode_errors.load();
            const float video_gap_pct =
                rtp_video > 0 ? (100.0f * static_cast<float>(video_missing) /
                                 static_cast<float>(rtp_video)) : 0.0f;
            const float perf_window_sec =
                std::chrono::duration<float>(now - last_perf_log).count();
            const float perf_window_fps =
                perf_window_sec > 0.01f
                    ? static_cast<float>(rendered - last_perf_rendered) / perf_window_sec
                    : 0.0f;
            lunar::diagnosticLog("perf",
                                 "stream fps=%.1f rendered=%u video_aus=%u audio_aus=%u "
                                 "rtp_video=%u rtp_audio=%u video_seq_gaps=%u "
                                 "missing=%u(%.3f%%) detected=%u recovered=%u "
                                 "unrecovered=%u quality=%s baseline_rtt=%u "
                                 "rtt_inflation=%u received_kbps=%u "
                                 "audio_seq_gaps=%u missing=%u "
                                 "h264_ok=%u h264_corrupt=%u h264_unsupported=%u h264_overflow=%u "
                                 "rtp_queue_drop=%u rtp_queue_high=%u rtp_queue_depth=%u "
                                 "rtp_queue_oldest_ms=%u udp_rcvbuf=%u "
                                 "srtp_fail=%u/%u srtp_detail=%u/%u/%u/%u "
                                 "ice_pair=%s:%u(%s)->%s:%u(%s) ice_rtt=%u "
                                 "nacks=%u retries=%u decode_errors=%u "
                                 "avg_decode_ms=%.2f avg_render_ms=%.2f",
                                 perf_window_fps,
                                 rendered,
                                 video_aus,
                                 audio_aus,
                                 rtp_video,
                                 rtp_audio,
                                 video_gaps,
                                 video_missing,
                                 video_gap_pct,
                                 media_stats.video_rtp_missing_packets_detected,
                                 media_stats.video_rtp_missing_packets_recovered,
                                 media_stats.video_rtp_missing_packets_unrecovered,
                                 webrtc::networkPathQualityName(
                                     media_stats.network_path.quality),
                                 media_stats.network_path.baseline_rtt_ms,
                                 media_stats.network_path.rtt_inflation_ms,
                                 media_stats.network_path.received_bitrate_kbps,
                                 audio_gaps,
                                 audio_missing,
                                 h264_ok,
                                 h264_corrupt,
                                 h264_unsupported,
                                 h264_overflow,
                                 queue_drops,
                                 queue_high,
                                 media_stats.rtp_queue_depth,
                                 media_stats.rtp_queue_oldest_age_ms,
                                 media_stats.udp_receive_buffer_bytes,
                                 srtp_rtp_fail,
                                 srtp_rtcp_fail,
                                 media_stats.srtp_rtp_auth_failures,
                                 media_stats.srtp_rtp_replay_failures,
                                 media_stats.srtp_rtp_replay_old_failures,
                                 media_stats.srtp_rtp_other_failures,
                                 media_stats.ice_pair_selected
                                     ? media_stats.ice_local_address : "none",
                                 static_cast<unsigned>(
                                     media_stats.ice_local_port),
                                 media_stats.ice_pair_selected
                                     ? iceCandidateTypeName(
                                           media_stats.ice_local_candidate_type)
                                     : "none",
                                 media_stats.ice_pair_selected
                                     ? media_stats.ice_remote_address : "none",
                                 static_cast<unsigned>(
                                     media_stats.ice_remote_port),
                                 media_stats.ice_pair_selected
                                     ? iceCandidateTypeName(
                                           media_stats.ice_remote_candidate_type)
                                     : "none",
                                 media_stats.ice_rtt_ms,
                                 media_stats.video_rtp_nacks,
                                 media_stats.video_rtp_nack_retries,
                                 decode_errors,
                                 perf_.avg_decode_ms(),
                                 perf_.avg_render_submit_ms());
            last_perf_log = now;
            last_perf_rendered = rendered;
        }

        if (!connected && reconnect_count < 5) {
            const int backoff_seconds = 1 << reconnect_count;
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_reconnect).count() >= backoff_seconds) {
                std::fprintf(stderr, "[ctrl] Reconnect %d/5\n", reconnect_count + 1);
                const bool reconnected = reconnectWithFreshSession(
                    profile, session_id, keep_alive_seconds, callbacks,
                    reconnect_prepared, reconnect_reason);
                if (reconnected) {
                    control_started = false;
                    video_watchdog_started = {};
                    source_discontinuity_baseline = 0;
                    renderer_recovery_attempts = 0;
                    decoder_recovery_attempts = 0;
                    last_health_recovery = {};
                    reconnect_count = 0;
                    reconnect_prepared = false;
                    reconnect_reason = "connection-state";
                }
                if (!reconnected) ++reconnect_count;
                last_reconnect = now;
            }
        } else if (!connected && reconnect_count >= 5) {
            streaming_ = false;
            lunar::diagnosticLog("xbox-stream", "Connection lost after reconnect exhaustion");
            notify(callbacks.on_error, "Connection lost. Reconnect attempts failed.");
            break;
        } else if (connected) {
            reconnect_count = 0;
        }

        if (reconnect_count > 0 && !connected) {
            next_network_tick = std::chrono::steady_clock::now();
            if (!sleepUnlessCancelled(std::chrono::milliseconds(100), callbacks)) break;
        } else {
            const auto loop_done = std::chrono::steady_clock::now();
            next_network_tick += kNetworkPumpInterval;
            if (next_network_tick < loop_done) next_network_tick = loop_done;
            if (!sleepUntilCancelled(next_network_tick, callbacks)) break;
        }
    }
    } catch (const std::exception& e) {
        lunar::diagnosticLog("xbox-stream", "runLoop exception: %s", e.what());
        std::fprintf(stderr, "[xbox-stream] runLoop exception: %s\n", e.what());
        streaming_ = false;
        notify(callbacks.on_error, std::string("Stream loop failed: ") + e.what());
    } catch (...) {
        lunar::diagnosticLog("xbox-stream", "runLoop unknown exception");
        std::fprintf(stderr, "[xbox-stream] runLoop unknown exception\n");
        streaming_ = false;
        notify(callbacks.on_error, "Stream loop failed: unknown exception");
    }
    streaming_ = false;
    control_cv_.notify_all();
    lunar::diagnosticLog("xbox-stream", "runLoop end");
}

void XboxStreamSession::controlLoop(std::string session_id,
                                    int keep_alive_seconds,
                                    RuntimeCallbacks callbacks) {
    const auto keep_alive_interval =
        std::chrono::seconds(std::max(1, keep_alive_seconds));
    constexpr auto token_refresh_interval = std::chrono::minutes(15);
    auto next_keep_alive = std::chrono::steady_clock::now() + keep_alive_interval;
    auto next_token_refresh =
        std::chrono::steady_clock::now() + token_refresh_interval;
    auto cancel = [this, callbacks]() {
        return !streaming_.load() || isCancelled(callbacks);
    };

    lunar::diagnosticLog("xbox-stream", "controlLoop begin session=%s keep_alive=%d",
                         session_id.c_str(),
                         keep_alive_seconds);
    try {
        while (streaming_.load() && !isCancelled(callbacks)) {
            const auto next_deadline = std::min(next_keep_alive, next_token_refresh);
            {
                std::unique_lock<std::mutex> lock(control_mutex_);
                control_cv_.wait_until(lock, next_deadline, [this, &callbacks]() {
                    return !streaming_.load() || isCancelled(callbacks);
                });
            }
            if (!streaming_.load() || isCancelled(callbacks)) {
                break;
            }

            auto now = std::chrono::steady_clock::now();
            if (now >= next_keep_alive && streaming_.load() &&
                !isCancelled(callbacks)) {
                const auto keep_alive_started = std::chrono::steady_clock::now();
                api::KeepAliveResult keep_alive_result;
                try {
                    std::lock_guard<std::mutex> api_lock(session_api_mutex_);
                    std::string active_session_id;
                    {
                        std::lock_guard<std::mutex> state_lock(state_mutex_);
                        active_session_id = session_id_;
                    }
                    if (active_session_id.empty()) {
                        // The stream worker is between fresh session
                        // associations. Do not send keep-alive for the old
                        // session while it is being replaced.
                        keep_alive_result.ok = true;
                    } else if (streaming_.load() && !isCancelled(callbacks)) {
                        keep_alive_result = session_client_.keepAliveDetailed(
                            active_session_id, cancel);
                        if (!keep_alive_result.ok && keep_alive_result.isAuthError() &&
                            callbacks.refresh_tokens && streaming_.load() &&
                            !isCancelled(callbacks)) {
                            lunar::diagnosticLog(
                                "xbox-stream",
                                "Keep-alive auth failure status=%d; refreshing streaming token",
                                keep_alive_result.status_code);
                            const bool refreshed = callbacks.refresh_tokens(true, cancel);
                            if (refreshed && streaming_.load() &&
                                !isCancelled(callbacks)) {
                                keep_alive_result =
                                    session_client_.keepAliveDetailed(active_session_id, cancel);
                                lunar::diagnosticLog(
                                    "xbox-stream",
                                    "Keep-alive retry after token refresh result=%s status=%d",
                                    keep_alive_result.ok ? "success" : "failed",
                                    keep_alive_result.status_code);
                            } else {
                                lunar::diagnosticLog(
                                    "xbox-stream",
                                    "Keep-alive token refresh failed; preserving media session");
                            }
                        }
                    }
                } catch (const std::exception& e) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
                    perf_.recordKeepAliveException();
#endif
                    lunar::diagnosticLog("xbox-stream",
                                         "Keep-alive exception: %s",
                                         e.what());
                } catch (...) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
                    perf_.recordKeepAliveException();
#endif
                    lunar::diagnosticLog("xbox-stream",
                                         "Keep-alive unknown exception");
                }
                const auto keep_alive_duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - keep_alive_started).count();
                perf_.recordKeepAlive(
                    static_cast<uint32_t>(std::max<int64_t>(0, keep_alive_duration)),
                    keep_alive_result.ok);
                if (!keep_alive_result.ok &&
                    keep_alive_result.isTerminalSessionError()) {
                    lunar::diagnosticLog(
                        "xbox-stream",
                        "Keep-alive reports session terminal status=%d; waiting for media liveness before reconnect",
                        keep_alive_result.status_code);
                } else if (!keep_alive_result.ok && !keep_alive_result.isAuthError()) {
                    lunar::diagnosticLog(
                        "xbox-stream",
                        "Keep-alive failed status=%d network=%s; preserving WebRTC media",
                        keep_alive_result.status_code,
                        keep_alive_result.network_error ? "true" : "false");
                }
                next_keep_alive = now + keep_alive_interval;
            }

            if (now >= next_token_refresh) {
                if (callbacks.refresh_tokens) {
                    bool refresh_ok = true;
                    try {
                        std::lock_guard<std::mutex> api_lock(session_api_mutex_);
                        if (streaming_.load() && !isCancelled(callbacks)) {
                            refresh_ok = callbacks.refresh_tokens(false, cancel);
                        }
                    } catch (const std::exception& e) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
                        perf_.recordTokenRefreshException();
#endif
                        lunar::diagnosticLog("xbox-stream",
                                             "Token refresh exception: %s",
                                             e.what());
                        refresh_ok = false;
                    } catch (...) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
                        perf_.recordTokenRefreshException();
#endif
                        lunar::diagnosticLog("xbox-stream",
                                             "Token refresh unknown exception");
                        refresh_ok = false;
                    }
                    if (!refresh_ok) {
                        lunar::persistentEventLog(
                            "xbox-stream",
                            "token refresh failed action=preserve-media");
                        lunar::diagnosticLog(
                            "xbox-stream",
                            "Periodic token refresh failed; preserving WebRTC media");
                    }
                }
                next_token_refresh = now + token_refresh_interval;
            }
        }
    } catch (const std::exception& e) {
        lunar::diagnosticLog("xbox-stream", "controlLoop exception: %s", e.what());
    } catch (...) {
        lunar::diagnosticLog("xbox-stream", "controlLoop unknown exception");
    }
    lunar::diagnosticLog("xbox-stream", "controlLoop end");
}

void XboxStreamSession::cleanupResources(bool delete_session) {
    const auto cleanup_started_at = std::chrono::steady_clock::now();
    lunar::diagnosticLog("xbox-stream", "cleanup begin delete_session=%s",
                         delete_session ? "true" : "false");
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        session_id = session_id_;
        session_id_.clear();
    }

    streaming_ = false;
    auto phase_started_at = std::chrono::steady_clock::now();
    lunar::persistentEventLog("xbox-stream", "cleanup phase=channels begin");
    channels_.reset();
    auto phase_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - phase_started_at);
    lunar::persistentEventLog(
        "xbox-stream", "cleanup phase=channels done elapsed_ms=%lld slow=%s",
        static_cast<long long>(phase_elapsed.count()),
        phase_elapsed >= std::chrono::seconds(3) ? "true" : "false");
    phase_started_at = std::chrono::steady_clock::now();
    lunar::persistentEventLog("xbox-stream", "cleanup phase=session-delete begin");
    if (delete_session && !session_id.empty()) {
        session_client_.deleteSessionAsync(session_id);
    }
    phase_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - phase_started_at);
    lunar::persistentEventLog(
        "xbox-stream", "cleanup phase=session-delete done elapsed_ms=%lld slow=%s",
        static_cast<long long>(phase_elapsed.count()),
        phase_elapsed >= std::chrono::seconds(3) ? "true" : "false");
    phase_started_at = std::chrono::steady_clock::now();
    lunar::persistentEventLog("xbox-stream", "cleanup phase=transport begin");
    transport_.disconnect();
    phase_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - phase_started_at);
    lunar::persistentEventLog(
        "xbox-stream", "cleanup phase=transport done elapsed_ms=%lld slow=%s",
        static_cast<long long>(phase_elapsed.count()),
        phase_elapsed >= std::chrono::seconds(3) ? "true" : "false");
    phase_started_at = std::chrono::steady_clock::now();
    lunar::persistentEventLog("xbox-stream", "cleanup phase=media begin");
    media_.shutdown();
    phase_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - phase_started_at);
    lunar::persistentEventLog(
        "xbox-stream", "cleanup phase=media done elapsed_ms=%lld slow=%s",
        static_cast<long long>(phase_elapsed.count()),
        phase_elapsed >= std::chrono::seconds(3) ? "true" : "false");
    phase_started_at = std::chrono::steady_clock::now();
    lunar::persistentEventLog("xbox-stream", "cleanup phase=input begin");
    rumble_.stop();
    gamepad_.releaseCaptureButton();
    phase_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - phase_started_at);
    lunar::persistentEventLog(
        "xbox-stream", "cleanup phase=input done elapsed_ms=%lld slow=%s",
        static_cast<long long>(phase_elapsed.count()),
        phase_elapsed >= std::chrono::seconds(3) ? "true" : "false");
    lunar::cloud1080CrashProbeLog(
        "crash-probe", "DEBUG-c1080 phase=session-cleanup normal=1");
    lunar::setCloud1080CrashProbeEnabled(false);
    const auto writer_stats = lunar::asyncDiagnosticWriterStats();
    lunar::latencyDiagnosticLog(
        "session",
        "phase=end writer_enqueued=%llu writer_dropped=%llu "
        "writer_batches=%llu writer_bytes=%llu writer_write_total_us=%llu "
        "writer_write_max_us=%llu writer_file_opens=%llu "
        "writer_flushes=%llu writer_queue_depth=%u writer_queue_high=%u",
        static_cast<unsigned long long>(writer_stats.enqueued),
        static_cast<unsigned long long>(writer_stats.dropped),
        static_cast<unsigned long long>(writer_stats.batches),
        static_cast<unsigned long long>(writer_stats.bytes_written),
        static_cast<unsigned long long>(writer_stats.write_total_us),
        static_cast<unsigned long long>(writer_stats.write_max_us),
        static_cast<unsigned long long>(writer_stats.file_opens),
        static_cast<unsigned long long>(writer_stats.flushes),
        writer_stats.queue_depth,
        writer_stats.queue_high_watermark);
    phase_started_at = std::chrono::steady_clock::now();
    lunar::persistentEventLog("xbox-stream",
                              "cleanup phase=diagnostics-writer begin");
    lunar::stopDropDiagnosticWriter();
    phase_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - phase_started_at);
    lunar::persistentEventLog(
        "xbox-stream",
        "cleanup phase=diagnostics-writer done elapsed_ms=%lld slow=%s",
        static_cast<long long>(phase_elapsed.count()),
        phase_elapsed >= std::chrono::seconds(3) ? "true" : "false");
    const auto cleanup_total = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cleanup_started_at);
    lunar::persistentEventLog(
        "xbox-stream", "cleanup complete total_ms=%lld slow=%s",
        static_cast<long long>(cleanup_total.count()),
        cleanup_total >= std::chrono::seconds(3) ? "true" : "false");
    lunar::diagnosticLog("xbox-stream", "cleanup done");
}

bool XboxStreamSession::failStart(const std::string& reason,
                                  const RuntimeCallbacks& callbacks) {
    lunar::diagnosticLog("xbox-stream", "Start failed: %s", reason.c_str());
    cleanupResources(true);
    notify(callbacks.on_error, reason);
    return false;
}

bool XboxStreamSession::cancelStart(const RuntimeCallbacks& callbacks) {
    lunar::diagnosticLog("xbox-stream", "Start cancelled");
    cleanupResources(true);
    notify(callbacks.on_cancelled, "Connection cancelled");
    return false;
}

} // namespace lunar::app
