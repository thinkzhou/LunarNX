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
constexpr std::chrono::milliseconds kInputPollInterval{16};
constexpr std::chrono::seconds kDataChannelTimeout{45};
constexpr std::chrono::seconds kStartupKeyframeRetryInterval{1};
constexpr std::chrono::seconds kRecoveryKeyframeInterval{1};
constexpr std::chrono::seconds kReceiverFeedbackInterval{1};
constexpr uint32_t kRecoveryMissingPacketsThreshold = 12;
constexpr uint32_t kRecoveryCorruptFramesThreshold = 4;

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
                                     stream::PerfStats& perf)
    : session_client_(session_client),
      transport_(transport),
      channels_(channels),
      media_(media),
      gamepad_(gamepad),
      xinput_(xinput),
      rumble_(rumble),
      perf_(perf) {}

XboxStreamSession::~XboxStreamSession() {
    stop();
}

bool XboxStreamSession::start(const StreamProfile& profile,
                              const stream::MediaPipelineOptions& media_options,
                              RuntimeCallbacks callbacks) {
    stop();
    stop_requested_ = false;
    channels_.reset();

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
    xinput_.reset();

    streaming_ = true;
    if (callbacks.on_streaming) {
        callbacks.on_streaming();
    }

    const int keep_alive_seconds =
        session.config.keep_alive_seconds > 0 ? session.config.keep_alive_seconds : 300;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        lunar::diagnosticLog("xbox-stream", "Stream thread create begin");
        stream_thread_ = std::thread(&XboxStreamSession::runLoop,
                                     this,
                                     profile,
                                     session.session_id,
                                     keep_alive_seconds,
                                     std::move(callbacks));
        lunar::diagnosticLog("xbox-stream", "Stream thread create done");
    }
    return true;
}

void XboxStreamSession::stop(bool delete_session) {
    stop_requested_ = true;
    streaming_ = false;

    std::thread thread_to_join;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (stream_thread_.joinable()) {
            thread_to_join = std::move(stream_thread_);
        }
    }
    if (thread_to_join.joinable() &&
        thread_to_join.get_id() != std::this_thread::get_id()) {
        thread_to_join.join();
    } else if (thread_to_join.joinable()) {
        thread_to_join.detach();
    }

    cleanupResources(delete_session);
}

std::string XboxStreamSession::sessionId() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return session_id_;
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
                                            : "waiting for recovery IDR");
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
    auto last_keep_alive = std::chrono::steady_clock::now();
    auto last_token_refresh = std::chrono::steady_clock::now();
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
    int input_send_failure_logs = 0;
    bool control_started = false;
    bool first_loop_logged = false;
    bool first_input_logged = false;
    auto next_input_tick = std::chrono::steady_clock::now();

    auto cancel = [this, callbacks]() { return isCancelled(callbacks); };

    try {
    while (streaming_.load() && !isCancelled(callbacks)) {
        if (!first_loop_logged) {
            lunar::diagnosticLog("xbox-stream", "runLoop first iteration connected=%s data_ready=%s",
                                 transport_.isConnected() ? "true" : "false",
                                 transport_.isDataChannelReady() ? "true" : "false");
            first_loop_logged = true;
        }
        const bool input_connected = transport_.isConnected();
        input::GamepadState gamepad_state{};
        try {
            gamepad_state = gamepad_.read();
        } catch (...) {
            // Keep zeroed state.
        }
        const bool guide_requested = control_started && input_connected &&
                                     callbacks.consume_guide_button &&
                                     callbacks.consume_guide_button();
        if (guide_requested) {
            // Send an unambiguous one-frame Nexus pulse. This also prevents
            // the menu activation button from leaking into the game.
            gamepad_state = {};
            gamepad_state.guide = true;
        } else if (callbacks.input_suppressed && callbacks.input_suppressed()) {
            gamepad_state.view = false;
            gamepad_state.menu = false;
        }
        const auto input_packet = xinput_.encode(gamepad_state);
        if (control_started && !first_input_logged) {
            lunar::diagnosticLog("xbox-stream", "first input path connected=%s packet_len=%zu",
                                 input_connected ? "true" : "false", input_packet.size());
            std::fprintf(stderr, "[xbox-stream] first input path connected=%s packet_len=%zu\n",
                         input_connected ? "true" : "false", input_packet.size());
            first_input_logged = true;
        }
        if (control_started && input_connected) {
            if (channels_.sendInputPacket(input_packet.data(), input_packet.size())) {
                perf_.recordInputPacket();
            } else if (input_send_failure_logs < 8) {
                lunar::diagnosticLog("xbox-stream",
                                     "input send failed sequence_attempt=%d",
                                     input_send_failure_logs + 1);
                input_send_failure_logs++;
            }
        }

        transport_.processEvents();
        const bool connected = transport_.isConnected();
        const auto media_stats = transport_.getMediaStats();
        const bool pipeline_recovery_pending = media_.hasVideoRecoveryRequest();
        perf_.setRtpStats(media_stats.video_rtp_packets,
                          media_stats.audio_rtp_packets,
                          media_stats.video_rtp_sequence_gaps,
                          media_stats.audio_rtp_sequence_gaps,
                          media_stats.video_rtp_missing_packets,
                          media_stats.audio_rtp_missing_packets,
                          media_stats.video_h264_frames,
                          media_stats.video_h264_corrupt_frames,
                          media_stats.video_h264_unsupported_nalus,
                          media_stats.video_h264_overflow_frames,
                          media_stats.video_h264_max_frame_bytes,
                          media_stats.rtp_queue_drops,
                          media_stats.rtp_queue_high_watermark,
                          media_stats.srtp_rtp_decrypt_failures,
                          media_stats.srtp_rtcp_decrypt_failures,
                          media_stats.ice_rtt_ms);

        if (control_started && connected &&
            (last_receiver_feedback.time_since_epoch().count() == 0 ||
             std::chrono::steady_clock::now() - last_receiver_feedback >=
                 kReceiverFeedbackInterval)) {
            const uint32_t bitrate_bps = static_cast<uint32_t>(
                streamProfileBitrateKbps(profile)) * 1000u;
            transport_.sendReceiverFeedback(bitrate_bps);
            last_receiver_feedback = std::chrono::steady_clock::now();
        }
        try {
            rumble_.update();
        } catch (const std::exception& e) {
            lunar::diagnosticLog("xbox-stream", "rumble update exception: %s", e.what());
        } catch (...) {
            lunar::diagnosticLog("xbox-stream", "rumble update unknown exception");
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
                try {
                    // Enable RTP handling and request keyframe once after auth.
                    transport_.setMediaEnabled(true);
                    std::fprintf(stderr, "[xbox-stream] media enabled\n");
                    const bool keyframe_requested =
                        channels_.requestVideoKeyframe(false);
                    std::fprintf(stderr, "[xbox-stream] initial keyframe=%s\n",
                                 keyframe_requested ? "true" : "false");
                    lunar::diagnosticLog("xbox-stream", "initial keyframe request result=%s",
                                         keyframe_requested ? "true" : "false");
                    last_keyframe_request = std::chrono::steady_clock::now();
                    keyframe_missing_baseline = media_stats.video_rtp_missing_packets;
                    keyframe_corrupt_baseline = media_stats.video_h264_corrupt_frames;
                    keyframe_queue_drop_baseline = media_stats.rtp_queue_drops;
                    keyframe_srtp_baseline = media_stats.srtp_rtp_decrypt_failures;
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[xbox-stream] post-control media exception: %s\n", e.what());
                    lunar::diagnosticLog("xbox-stream", "post-control media exception: %s", e.what());
                    // Keep control_started true; media enable failure should not kill session.
                } catch (...) {
                    std::fprintf(stderr, "[xbox-stream] post-control media unknown exception\n");
                    lunar::diagnosticLog("xbox-stream", "post-control media unknown exception");
                }
            }
            if (!control_started && isCancelled(callbacks)) {
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (control_started && connected) {
            const uint32_t missing_delta =
                media_stats.video_rtp_missing_packets - keyframe_missing_baseline;
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
                keyframe_missing_baseline = media_stats.video_rtp_missing_packets;
                keyframe_corrupt_baseline = media_stats.video_h264_corrupt_frames;
                keyframe_queue_drop_baseline = media_stats.rtp_queue_drops;
                keyframe_srtp_baseline = media_stats.srtp_rtp_decrypt_failures;
            } else if ((video_damage_increased || pipeline_recovery_pending) &&
                       can_request_recovery_keyframe) {
                const bool keyframe_requested =
                    channels_.requestVideoKeyframe(false);
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
                    keyframe_missing_baseline = media_stats.video_rtp_missing_packets;
                    keyframe_corrupt_baseline = media_stats.video_h264_corrupt_frames;
                    keyframe_queue_drop_baseline = media_stats.rtp_queue_drops;
                    keyframe_srtp_baseline = media_stats.srtp_rtp_decrypt_failures;
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
                                 "rtp_video=%u rtp_audio=%u video_seq_gaps=%u missing=%u(%.3f%%) "
                                 "audio_seq_gaps=%u missing=%u "
                                 "h264_ok=%u h264_corrupt=%u h264_unsupported=%u h264_overflow=%u "
                                 "rtp_queue_drop=%u rtp_queue_high=%u "
                                 "srtp_fail=%u/%u decode_errors=%u "
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
                                 audio_gaps,
                                 audio_missing,
                                 h264_ok,
                                 h264_corrupt,
                                 h264_unsupported,
                                 h264_overflow,
                                 queue_drops,
                                 queue_high,
                                 srtp_rtp_fail,
                                 srtp_rtcp_fail,
                                 decode_errors,
                                 perf_.avg_decode_ms(),
                                 perf_.avg_render_submit_ms());
            last_perf_log = now;
            last_perf_rendered = rendered;
        }

        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_keep_alive).count() >= keep_alive_seconds / 2) {
            session_client_.keepAlive(session_id, cancel);
            last_keep_alive = now;
        }

        if (std::chrono::duration_cast<std::chrono::minutes>(
                now - last_token_refresh).count() >= 15) {
            if (callbacks.refresh_tokens) {
                callbacks.refresh_tokens();
            }
            last_token_refresh = now;
        }

        if (!connected && reconnect_count < 5) {
            const int backoff_seconds = 1 << reconnect_count;
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_reconnect).count() >= backoff_seconds) {
                std::fprintf(stderr, "[ctrl] Reconnect %d/5\n", reconnect_count + 1);
                transport_.disconnect();
                if (transport_.initialize()) {
                    channels_.reset();
                    transport_.setCallbacks(createPeerCallbacks());
                    transport_.setMediaEnabled(false);
                    if (negotiateWebRtc(profile, session_id, callbacks) &&
                        transport_.waitDataChannels(kDataChannelTimeout, cancel)) {
                        xinput_.reset();
                        control_started = false;
                    }
                }
                ++reconnect_count;
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
            next_input_tick = std::chrono::steady_clock::now();
            if (!sleepUnlessCancelled(std::chrono::milliseconds(100), callbacks)) break;
        } else {
            next_input_tick += kInputPollInterval;
            const auto loop_done = std::chrono::steady_clock::now();
            if (next_input_tick < loop_done) next_input_tick = loop_done;
            if (!sleepUntilCancelled(next_input_tick, callbacks)) break;
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
    lunar::diagnosticLog("xbox-stream", "runLoop end");
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
    lunar::diagnosticLog("xbox-stream", "cleanup rumble stop done");
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
