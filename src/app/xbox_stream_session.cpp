#include "xbox_stream_session.h"
#include "../diagnostics.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <cstdio>

namespace lunar::app {

namespace {

constexpr std::chrono::milliseconds kIceStableWindow{800};
constexpr std::chrono::milliseconds kIceGatherTimeout{5000};
constexpr std::chrono::milliseconds kNetworkPumpInterval{2};
constexpr std::chrono::milliseconds kInputSampleInterval{8};
// Match XStreaming's 62.5 Hz baseline for unchanged controller snapshots.
// Button transitions are published immediately by the 8 ms producer.
constexpr std::chrono::milliseconds kInputSnapshotInterval{16};
// Reliable ordered input does not need an idle packet every tick. Keep the
// server-side pad state alive with a bounded heartbeat instead.
constexpr std::chrono::milliseconds kInputHeartbeatInterval{250};
constexpr std::chrono::seconds kDataChannelTimeout{45};
constexpr std::chrono::seconds kStartupKeyframeRetryInterval{1};
constexpr std::chrono::seconds kRecoveryKeyframeInterval{1};
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
    channels_.reset();
    transport_.setVideoJitterMode(
        profile.type == SessionType::Cloud
            ? webrtc::VideoJitterMode::Cloud
            : webrtc::VideoJitterMode::Home);

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

    notify(callbacks.on_status, "Initializing media pipeline...");
    if (isCancelled(callbacks)) {
        return cancelStart(callbacks);
    }
    lunar::diagnosticLog("xbox-stream", "Media init begin width=%d height=%d",
                         profile.width,
                         profile.height);
    if (!media_.initialize(profile.width, profile.height, &perf_, media_options)) {
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
    if (callbacks.on_streaming) {
        callbacks.on_streaming();
    }

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

    return true;
}

void XboxStreamSession::stop(bool delete_session) {
    stop_requested_ = true;
    streaming_ = false;
    input_loop_stop_ = true;
    input_delivery_ready_ = false;
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
    if (stream_thread_to_join.joinable()) {
        stream_thread_to_join.join();
    }
    if (control_thread_to_join.joinable()) {
        control_thread_to_join.join();
    }
    if (input_thread_to_join.joinable()) {
        input_thread_to_join.join();
    }

    cleanupResources(delete_session);
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
        auto last_input_owner = input_router_.owner();
        auto next_snapshot = std::chrono::steady_clock::now();
        auto next_heartbeat = next_snapshot;
        auto next_input_tick = next_snapshot;

        while (!input_loop_stop_.load() && streaming_.load() &&
               !isCancelled(callbacks)) {
            try {
                sampleInput(callbacks,
                            guide_pulse_frames_remaining,
                            guide_release_pending,
                            next_snapshot,
                            next_heartbeat,
                            last_input_owner);
            } catch (const std::exception& e) {
                lunar::diagnosticLog("xbox-input", "input sample exception: %s", e.what());
            } catch (...) {
                lunar::diagnosticLog("xbox-input", "input sample unknown exception");
            }

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
    bool& guide_release_pending,
    std::chrono::steady_clock::time_point& next_snapshot,
    std::chrono::steady_clock::time_point& next_heartbeat,
    input::StreamInputOwner& last_input_owner) {
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
    const auto input_owner = input_router_.owner();
    const bool owner_changed = input_owner != last_input_owner;
    last_input_owner = input_owner;
    gamepad_state = input_router_.route(gamepad_state);

    const auto sampled_at = std::chrono::steady_clock::now();
    const bool snapshot_due = sampled_at >= next_snapshot;
    const bool heartbeat_due = sampled_at >= next_heartbeat;
    input_accumulator_.publish(gamepad_state,
                               input_delivery_ready_.load(),
                               snapshot_due,
                               (owner_changed || heartbeat_due) &&
                                   input_delivery_ready_.load());
    if (snapshot_due) {
        next_snapshot = sampled_at + kInputSnapshotInterval;
    }
    if (heartbeat_due) {
        next_heartbeat = sampled_at + kInputHeartbeatInterval;
    }
}

void XboxStreamSession::prepareInputForReconnect() {
    input_delivery_ready_ = false;
    input_accumulator_.prepareForReconnect();
    lunar::persistentEventLog(
        "xbox-input", "input-reconnect-resync transition_depth=%zu",
        input_accumulator_.pendingTransitionCount());
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

bool XboxStreamSession::reconnectWithFreshSession(
    const StreamProfile& profile,
    std::string& session_id,
    int& keep_alive_seconds,
    const RuntimeCallbacks& callbacks) {
    auto cancel = [this, callbacks]() { return isCancelled(callbacks); };
    auto sleep = [this, callbacks](std::chrono::milliseconds duration) {
        return sleepUnlessCancelled(duration, callbacks);
    };

    const std::string old_session_id = session_id;
    prepareInputForReconnect();
    media_.prepareForNewVideoSource("fresh WebRTC association");
    transport_.disconnect();
    channels_.reset();
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

    control_recovery_requested_ = false;
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
    auto last_keyframe_request = std::chrono::steady_clock::time_point{};
    auto last_receiver_feedback = std::chrono::steady_clock::time_point{};
    uint32_t last_perf_rendered = 0;
    uint32_t keyframe_missing_baseline = 0;
    uint32_t keyframe_corrupt_baseline = 0;
    uint32_t keyframe_queue_drop_baseline = 0;
    uint32_t keyframe_srtp_baseline = 0;
    int reconnect_count = 0;
    uint32_t health_recovery_attempts = 0;
    auto last_health_recovery = std::chrono::steady_clock::time_point{};
    auto video_watchdog_started = std::chrono::steady_clock::time_point{};
    auto next_health_poll = std::chrono::steady_clock::now();
    stream::MediaHealthStats media_health{};
    std::optional<input::XboxInputAccumulator::Batch> pending_input_batch;
    uint64_t pending_input_ticket = 0;
    uint32_t source_discontinuity_baseline = 0;
    int input_send_failure_logs = 0;
    bool control_started = false;
    bool first_loop_logged = false;
    bool first_input_logged = false;
    auto next_network_tick = std::chrono::steady_clock::now();
    auto next_rumble_tick = next_network_tick;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    auto last_webrtc_pump = std::chrono::steady_clock::time_point{};
#endif
    auto cancel = [this, callbacks]() { return isCancelled(callbacks); };

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
        if (control_started && connected_before_pump &&
            !pending_input_batch) {
            auto input_batch = input_accumulator_.peekBatch();
            if (input_batch) {
                // Re-apply ownership at the send boundary. The sampling
                // thread also routes input, but UI ownership can change
                // between sampling and this network loop iteration.
                for (auto& gamepad_state : input_batch->frames) {
                    gamepad_state = input_router_.route(gamepad_state);
                }
                const auto input_packet = xinput_.encodeFrames(input_batch->frames);
                if (!first_input_logged) {
                    lunar::diagnosticLog(
                        "xbox-stream",
                        "first input path connected=true frames=%zu packet_len=%zu",
                        input_batch->frames.size(),
                        input_packet.size());
                    std::fprintf(
                        stderr,
                        "[xbox-stream] first input path connected=true frames=%zu packet_len=%zu\n",
                        input_batch->frames.size(),
                        input_packet.size());
                    first_input_logged = true;
                }
                const auto submission = channels_.sendInputPacket(
                    input_packet.data(), input_packet.size(),
                    input_batch->reliable);
                if (submission.accepted && submission.ticket != 0) {
                    pending_input_ticket = submission.ticket;
                    pending_input_batch = std::move(*input_batch);
                } else if (submission.accepted) {
                    input_accumulator_.commitBatch(*input_batch);
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
            next_rumble_tick += kInputSnapshotInterval;
            if (next_rumble_tick <= loop_started) {
                next_rumble_tick = loop_started + kInputSnapshotInterval;
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
        while (const auto delivery = transport_.consumeInputDeliveryResult()) {
            if (delivery->ticket != pending_input_ticket) continue;
            if (delivery->sent && pending_input_batch) {
                input_accumulator_.commitBatch(*pending_input_batch);
                perf_.recordInputPacket();
            } else if (!delivery->sent) {
                lunar::dropDiagnosticLog(
                    "xbox-input", "transition send failed ticket=%llu",
                    static_cast<unsigned long long>(delivery->ticket));
            }
            pending_input_batch.reset();
            pending_input_ticket = 0;
        }
        if (control_recovery_requested_.exchange(false)) {
            lunar::persistentEventLog(
                "xbox-stream",
                "WebRTC reconnect requested reason=control-plane-health");
            prepareInputForReconnect();
            media_.prepareForNewVideoSource("control-plane recovery");
            transport_.disconnect();
        }
        if (input_accumulator_.consumeOverflowFault()) {
            lunar::persistentEventLog(
                "xbox-input",
                "input-transition-overflow transition_depth=%zu action=reconnect",
                input_accumulator_.pendingTransitionCount());
            prepareInputForReconnect();
            transport_.disconnect();
        }
        if (transport_.consumeDataChannelFailure()) {
            lunar::diagnosticLog("xbox-stream",
                                 "DataChannel send budget exhausted; reconnecting");
            lunar::persistentEventLog(
                "xbox-stream",
                "WebRTC reconnect requested reason=data-channel-send-failure");
            prepareInputForReconnect();
            transport_.disconnect();
        }
        const bool connected = transport_.isConnected();
        if (!connected && input_delivery_ready_.exchange(false)) {
            input_accumulator_.prepareForReconnect();
        }
        if (!connected) {
            pending_input_batch.reset();
            pending_input_ticket = 0;
        }
        const auto media_stats = transport_.getMediaStats();
        const auto health_poll_now = std::chrono::steady_clock::now();
        const bool health_poll_due = health_poll_now >= next_health_poll;
        if (health_poll_due) {
            media_health = media_.getHealthStats();
            next_health_poll = health_poll_now + kMediaHealthPollInterval;
        }
        const bool pipeline_recovery_pending = media_.hasVideoRecoveryRequest();
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
            const uint32_t bitrate_bps = static_cast<uint32_t>(
                streamProfileBitrateKbps(profile)) * 1000u;
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
                control_started = true;
                video_watchdog_started = std::chrono::steady_clock::now();
                next_health_poll = video_watchdog_started;
                input_accumulator_.prepareForReconnect();
                input_delivery_ready_ = true;
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
                    keyframe_missing_baseline =
                        media_stats.video_rtp_missing_packets_detected;
                    keyframe_corrupt_baseline = media_stats.video_h264_corrupt_frames;
                    keyframe_queue_drop_baseline = media_stats.rtp_queue_drops;
                    keyframe_srtp_baseline = media_stats.srtp_rtp_decrypt_failures;
                } catch (const std::exception& e) {
                    lunar::dropDiagnosticLog(
                        "xbox-stream", "post-control media exception: %s", e.what());
                    // Keep control_started true; media enable failure should not kill session.
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
        const uint32_t source_discontinuities =
            media_stats.video_rtp_ssrc_changes +
            media_stats.video_rtp_timestamp_discontinuities;
        if (source_discontinuities > source_discontinuity_baseline) {
            source_discontinuity_baseline = source_discontinuities;
            video_watchdog_started = now;
            health_recovery_attempts = 0;
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
            const bool can_request_recovery_keyframe =
                last_keyframe_request.time_since_epoch().count() == 0 ||
                now - last_keyframe_request >= kRecoveryKeyframeInterval;
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
            } else if ((video_damage_increased || pipeline_recovery_pending ||
                        media_stats.video_waiting_keyframe) &&
                       can_request_recovery_keyframe) {
                const bool keyframe_requested =
                    channels_.requestVideoKeyframe(false);
                perf_.recordRecoveryPli(keyframe_requested);
                lunar::diagnosticLog("xbox-stream",
                                     "recovery keyframe request result=%s pipeline=%s missing=%u(+%u) corrupt=%u(+%u) queue_drop=%u(+%u) srtp=%u(+%u)",
                                     keyframe_requested ? "true" : "false",
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
                    last_keyframe_request = now;
                    keyframe_missing_baseline =
                        media_stats.video_rtp_missing_packets_detected;
                    keyframe_corrupt_baseline = media_stats.video_h264_corrupt_frames;
                    keyframe_queue_drop_baseline = media_stats.rtp_queue_drops;
                    keyframe_srtp_baseline = media_stats.srtp_rtp_decrypt_failures;
                }
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
            if (rtp_stalled) {
                lunar::persistentEventLog(
                    "xbox-stream",
                    "video watchdog stalled rtp_age_ms=%u decoded_age_ms=%llu "
                    "action=fresh-session-reconnect",
                    media_stats.video_rtp_arrival_age_ms,
                    static_cast<unsigned long long>(media_health.decoded_video_age_ms));
                prepareInputForReconnect();
                media_.prepareForNewVideoSource("video RTP liveness timeout");
                transport_.disconnect();
            } else if (health_poll_due) {
                const bool rtp_alive = rtp_seen &&
                    media_stats.video_rtp_arrival_age_ms <
                        static_cast<uint32_t>(kVideoRtpStallTimeout.count());
                const bool decode_stalled = rtp_alive && watchdog_started &&
                    ((!media_health.has_decoded_video &&
                      video_watchdog_age >= kVideoDecodeStallTimeout) ||
                     (media_health.has_decoded_video &&
                      media_health.decoded_video_age_ms >=
                          static_cast<uint64_t>(kVideoDecodeStallTimeout.count())));
                const bool present_stalled =
                    media_health.has_decoded_video &&
                    ((media_health.has_presented_video &&
                      media_health.presented_video_age_ms >=
                          static_cast<uint64_t>(kVideoPresentStallTimeout.count())) ||
                     (!media_health.has_presented_video &&
                      media_health.decoded_video_age_ms >=
                          static_cast<uint64_t>(kVideoPresentStallTimeout.count())) ||
                     media_health.consecutive_render_faults >= 2);
                const bool pipeline_stalled = decode_stalled || present_stalled;
                if (pipeline_stalled) {
                    if (last_health_recovery.time_since_epoch().count() == 0 ||
                        now - last_health_recovery >= kVideoHealthRecoveryCooldown) {
                        if (health_recovery_attempts >= 1) {
                            lunar::persistentEventLog(
                                "xbox-stream",
                                "video watchdog persistent pipeline stall decode_age_ms=%llu "
                                "present_age_ms=%llu faults=%u action=fresh-session-reconnect",
                                static_cast<unsigned long long>(media_health.decoded_video_age_ms),
                                static_cast<unsigned long long>(media_health.presented_video_age_ms),
                                media_health.render_fault_count);
                            prepareInputForReconnect();
                            media_.prepareForNewVideoSource(
                                "persistent video pipeline stall");
                            transport_.disconnect();
                        } else {
                            lunar::persistentEventLog(
                                "xbox-stream",
                                "video watchdog pipeline stall decode_age_ms=%llu "
                                "present_age_ms=%llu faults=%u action=decoder-recovery",
                                static_cast<unsigned long long>(media_health.decoded_video_age_ms),
                                static_cast<unsigned long long>(media_health.presented_video_age_ms),
                                media_health.render_fault_count);
                            media_.requestVideoRecovery(
                                decode_stalled ? "video decode watchdog"
                                               : "video present watchdog",
                                true);
                            ++health_recovery_attempts;
                            last_health_recovery = now;
                        }
                    }
                } else {
                    if (media_health.has_presented_video &&
                        media_health.presented_video_age_ms < 1000 &&
                        media_health.consecutive_render_faults == 0) {
                        health_recovery_attempts = 0;
                    }
                }
            }
        }

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
                    profile, session_id, keep_alive_seconds, callbacks);
                if (reconnected) {
                    control_started = false;
                    video_watchdog_started = {};
                    source_discontinuity_baseline = 0;
                    health_recovery_attempts = 0;
                    last_health_recovery = {};
                    reconnect_count = 0;
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
        std::chrono::seconds(std::max(1, keep_alive_seconds / 2));
    constexpr auto token_refresh_interval = std::chrono::minutes(15);
    auto next_keep_alive = std::chrono::steady_clock::now() + keep_alive_interval;
    auto next_token_refresh =
        std::chrono::steady_clock::now() + token_refresh_interval;
    uint32_t keep_alive_failures = 0;
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
                bool keep_alive_ok = false;
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
                        keep_alive_ok = true;
                    } else if (streaming_.load() && !isCancelled(callbacks)) {
                        keep_alive_ok = session_client_.keepAlive(
                            active_session_id, cancel);
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
                    keep_alive_ok);
                if (keep_alive_ok) {
                    keep_alive_failures = 0;
                } else if (++keep_alive_failures >= 2) {
                    lunar::persistentEventLog(
                        "xbox-stream",
                        "keep-alive failed consecutive=%u action=reconnect",
                        keep_alive_failures);
                    control_recovery_requested_ = true;
                    control_cv_.notify_all();
                }
                next_keep_alive = now + keep_alive_interval;
            }

            if (now >= next_token_refresh) {
                if (callbacks.refresh_tokens) {
                    bool refresh_ok = true;
                    try {
                        std::lock_guard<std::mutex> api_lock(session_api_mutex_);
                        if (streaming_.load() && !isCancelled(callbacks)) {
                            refresh_ok = callbacks.refresh_tokens();
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
                            "token refresh failed action=reconnect");
                        control_recovery_requested_ = true;
                        control_cv_.notify_all();
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
    lunar::diagnosticLog("xbox-stream", "cleanup begin delete_session=%s",
                         delete_session ? "true" : "false");
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        session_id = session_id_;
        session_id_.clear();
    }

    streaming_ = false;
    lunar::diagnosticLog("xbox-stream", "cleanup channels reset begin");
    channels_.reset();
    lunar::diagnosticLog("xbox-stream", "cleanup channels reset done");
    if (delete_session && !session_id.empty()) {
        lunar::diagnosticLog("xbox-stream", "cleanup delete-session async begin id=%s",
                             session_id.c_str());
        session_client_.deleteSessionAsync(session_id);
        lunar::diagnosticLog("xbox-stream", "cleanup delete-session async done");
    }
    lunar::diagnosticLog("xbox-stream", "cleanup transport disconnect begin");
    transport_.disconnect();
    lunar::diagnosticLog("xbox-stream", "cleanup transport disconnect done");
    lunar::diagnosticLog("xbox-stream", "cleanup media shutdown begin");
    media_.shutdown();
    lunar::diagnosticLog("xbox-stream", "cleanup media shutdown done");
    lunar::diagnosticLog("xbox-stream", "cleanup rumble stop begin");
    rumble_.stop();
    gamepad_.releaseCaptureButton();
    lunar::diagnosticLog("xbox-stream", "cleanup rumble stop done");
    lunar::cloud1080CrashProbeLog(
        "crash-probe", "DEBUG-c1080 phase=session-cleanup normal=1");
    lunar::setCloud1080CrashProbeEnabled(false);
    lunar::stopDropDiagnosticWriter();
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
