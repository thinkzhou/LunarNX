#include "peer_manager.h"
#include "video_jitter_policy.h"
#include "xbox_input_feedback.h"
#include "xstreaming_data_channels.h"
#include "../diagnostics.h"
#include "../input/xinput_encoder.h"
#include <cJSON.h>
#include <peer.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cctype>

namespace lunar::webrtc {

namespace {
constexpr uint32_t kMaxReliableSendAttempts = 128;
constexpr uint32_t kMaxConsecutiveSctpSendFailures = 128;
constexpr std::chrono::milliseconds kMaxSctpRecoverableFailureAge{250};
constexpr std::chrono::milliseconds kMediaStatsCacheInterval{250};
constexpr std::array<const char*, 7> kXStreamingIceServers{
    "stun:worldaz.relay.teams.microsoft.com:3478",
    "stun:stun.l.google.com:19302",
    "stun:stun1.l.google.com:19302",
    "stun:relay1.expressturn.com",
    "stun:relay2.expressturn.com",
    "stun:stun.kinesisvideo.us-east-1.amazonaws.com:443",
    "stun:stun.douyucdn.cn:18000",
};
uint64_t elapsedNs(std::chrono::steady_clock::time_point start) {
    if (start.time_since_epoch().count() == 0) return 0;
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count());
}

std::string normalizeCandidateLine(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.rfind("candidate:", 0) == 0) s = "a=" + s;
    return s;
}

void appendCandidateFromJson(cJSON* item, std::vector<std::string>& candidates) {
    if (!item) return;
    cJSON* candidate = cJSON_IsObject(item) ? cJSON_GetObjectItem(item, "candidate") : item;
    if (candidate && cJSON_IsString(candidate) && candidate->valuestring) {
        auto line = normalizeCandidateLine(candidate->valuestring);
        if (line.rfind("a=candidate:", 0) == 0) candidates.push_back(std::move(line));
    }
}

std::vector<std::string> extractCandidateLines(const std::string& payload) {
    std::vector<std::string> candidates;

    cJSON* root = cJSON_Parse(payload.c_str());
    if (root) {
        if (cJSON_IsArray(root)) {
            int count = cJSON_GetArraySize(root);
            for (int i = 0; i < count; i++) appendCandidateFromJson(cJSON_GetArrayItem(root, i), candidates);
        } else if (cJSON_IsObject(root)) {
            cJSON* list = cJSON_GetObjectItem(root, "iceCandidates");
            if (!list) list = cJSON_GetObjectItem(root, "candidates");
            if (list && cJSON_IsArray(list)) {
                int count = cJSON_GetArraySize(list);
                for (int i = 0; i < count; i++) appendCandidateFromJson(cJSON_GetArrayItem(list, i), candidates);
            } else {
                appendCandidateFromJson(root, candidates);
            }
        }
        cJSON_Delete(root);
    }

    if (candidates.empty()) {
        auto line = normalizeCandidateLine(payload);
        if (line.rfind("a=candidate:", 0) == 0) candidates.push_back(std::move(line));
    }

    return candidates;
}

void logRemoteMediaDescription(const std::string& sdp) {
    bool in_video_section = false;
    size_t start = 0;
    while (start < sdp.size()) {
        size_t end = sdp.find('\n', start);
        std::string line = sdp.substr(start,
            end == std::string::npos ? std::string::npos : end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }

        if (line.rfind("m=", 0) == 0) {
            in_video_section = line.rfind("m=video ", 0) == 0;
        }

        const bool session_setup = line.rfind("a=setup:", 0) == 0;
        const bool video_detail = in_video_section &&
            (line.rfind("m=video ", 0) == 0 ||
             line.rfind("a=mid:", 0) == 0 ||
             line.rfind("a=rtpmap:", 0) == 0 ||
             line.rfind("a=fmtp:", 0) == 0 ||
             line.rfind("a=rtcp-fb:", 0) == 0 ||
             line.rfind("a=ssrc:", 0) == 0 ||
             line == "a=sendonly" || line == "a=recvonly" ||
             line == "a=sendrecv" || line == "a=inactive");
        if (session_setup || video_detail) {
            lunar::diagnosticLog("webrtc-sdp", "remote %s", line.c_str());
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
}

} // namespace

// =============================================================================
// Static callbacks
// =============================================================================
void PeerManager::onIceCandidate(char* sdp_text, void* userdata) {
    auto* self = static_cast<PeerManager*>(userdata);
    if (!sdp_text) return;

    // The callback gives us a full SDP string containing all gathered candidates.
    // Parse every a=candidate: line (not just the first).
    std::string sdp_str(sdp_text);
    size_t pos = 0;
    while ((pos = sdp_str.find("a=candidate:", pos)) != std::string::npos) {
        size_t end = sdp_str.find('\n', pos);
        std::string line = sdp_str.substr(pos, end - pos);

        // Strip "a=" prefix for the Xbox API (expects raw candidate string)
        if (line.size() > 2 && line[0] == 'a' && line[1] == '=')
            line = line.substr(2);

        // Skip duplicates
        bool dup = false;
        for (auto& c : self->local_candidates_) {
            if (c.sdp == line) { dup = true; break; }
        }
        if (!dup) {
            fprintf(stderr, "[webrtc] ICE candidate: %s\n", line.c_str());
            lunar::diagnosticLog("webrtc", "local ICE candidate: %s", line.c_str());
            IceCandidate c;
            c.sdp = line;
            c.sdp_mid = "0";
            c.sdp_mline_index = 0;
            self->local_candidates_.push_back(c);
        }
        pos = end;
    }
}

void PeerManager::onIceStateChange(PeerConnectionState state, void* userdata) {
    auto* self = static_cast<PeerManager*>(userdata);
    fprintf(stderr, "[webrtc] ICE state: %d (%s)\n", state,
            peer_connection_state_to_string(state));
    lunar::diagnosticLog("webrtc", "ICE state: %d (%s)",
                         state,
                         peer_connection_state_to_string(state));
    if (state == PEER_CONNECTION_CONNECTED || state == PEER_CONNECTION_COMPLETED) {
        self->connected_ = true;
    } else if (state == PEER_CONNECTION_DISCONNECTED ||
               state == PEER_CONNECTION_FAILED ||
               state == PEER_CONNECTION_CLOSED) {
        self->connected_ = false;
    }
}

void PeerManager::onVideoTrack(uint8_t* data,
                               size_t size,
                               uint16_t sequence,
                               uint32_t timestamp,
                               void* userdata) {
    auto* self = static_cast<PeerManager*>(userdata);
    const uint64_t arrival_ns = elapsedNs(self->media_clock_start_);
    int log_index = self->video_callback_logs_.fetch_add(1);
    const uint64_t packet_index = static_cast<uint64_t>(log_index) + 1;
    if (packet_index <= 4 && lunar::cloud1080CrashProbeEnabled()) {
        lunar::cloud1080CrashProbeLog(
            "crash-probe",
            "DEBUG-c1080 phase=rtp-received packet=%llu bytes=%zu seq=%u "
            "rtp_ts=%u arrival_ns=%llu",
            static_cast<unsigned long long>(packet_index),
            size,
            sequence,
            timestamp,
            static_cast<unsigned long long>(arrival_ns));
    }
    if (log_index < 8) {
        lunar::diagnosticLog("webrtc", "video callback begin len=%zu has_cb=%s",
                             size,
                             self->callbacks_.on_video_frame ? "true" : "false");
    }
    if (self->callbacks_.on_video_packet) {
        try {
            self->callbacks_.on_video_packet(size);
        } catch (...) {
        }
    }
    try {
        self->video_jitter_.receive(
            data,
            size,
            arrival_ns / 1000000u,
            [self, arrival_ns](const uint8_t* access_unit,
                               size_t access_unit_size,
                               uint16_t frame_sequence,
                               uint32_t frame_timestamp) {
                const uint64_t access_unit_index =
                    self->crash_probe_access_units_.fetch_add(1) + 1;
                if (lunar::shouldSampleCloud1080CrashProbe(access_unit_index)) {
                    lunar::cloud1080CrashProbeLog(
                        "crash-probe",
                        "DEBUG-c1080 phase=access-unit-complete au=%llu bytes=%zu "
                        "seq=%u rtp_ts=%u arrival_ns=%llu",
                        static_cast<unsigned long long>(access_unit_index),
                        access_unit_size,
                        frame_sequence,
                        frame_timestamp,
                        static_cast<unsigned long long>(arrival_ns));
                }
                const uint64_t anchor_arrival_ns =
                    self->video_clock_.anchored()
                        ? arrival_ns
                        : self->recoverRtpClockAnchorArrivalNs("video");
                const uint64_t mapped_timestamp =
                    self->video_clock_.map(frame_timestamp, anchor_arrival_ns);
                if (self->callbacks_.on_video_frame) {
                    self->callbacks_.on_video_frame(access_unit,
                                                    access_unit_size,
                                                    frame_sequence,
                                                    mapped_timestamp);
                }
            },
            [self](uint16_t pid, uint16_t blp) {
                const bool queued = self->enqueueNack(pid, blp);
                if (self->nack_logs_.fetch_add(1) < 32) {
                    lunar::diagnosticLog(
                        "webrtc",
                        "queue RTCP NACK pid=%u blp=0x%04x result=%s",
                        pid,
                        blp,
                        queued ? "true" : "false");
                }
                return queued;
            },
            [self](bool reset_decoder) {
                self->handleVideoJitterRecovery(reset_decoder);
            },
            [self](uint32_t ssrc) {
                self->video_clock_.reset();
                if (self->callbacks_.on_video_source_discontinuity) {
                    try {
                        self->callbacks_.on_video_source_discontinuity(ssrc);
                    } catch (...) {
                        lunar::diagnosticLog(
                            "webrtc", "video source discontinuity callback failed");
                    }
                }
            });
    } catch (...) {
        // std::function construction happens before VideoRtpJitterBuffer can
        // catch allocation failures. Never let a C++ exception cross libpeer's
        // C callback boundary.
        self->handleVideoJitterRecovery(true);
    }
    if (log_index < 8) {
        lunar::diagnosticLog("webrtc", "video callback done");
    }
}

void PeerManager::handleVideoJitterRecovery(bool reset_decoder) noexcept {
    lunar::cloud1080CrashProbeLog(
        "crash-probe",
        "DEBUG-c1080 phase=jitter-recovery reset_decoder=%d",
        reset_decoder ? 1 : 0);
    if (callbacks_.on_video_recovery) {
        try {
            callbacks_.on_video_recovery(reset_decoder);
        } catch (...) {
            lunar::diagnosticLog("webrtc", "video recovery callback failed");
        }
    }
}

void PeerManager::onAudioTrack(uint8_t* data,
                               size_t size,
                               uint16_t sequence,
                               uint32_t timestamp,
                               void* userdata) {
    auto* self = static_cast<PeerManager*>(userdata);
    const uint64_t callback_ns = elapsedNs(self->media_clock_start_);
    const uint64_t arrival_ns = self->audio_clock_.anchored()
        ? callback_ns
        : self->recoverRtpClockAnchorArrivalNs("audio");
    const uint64_t mapped_timestamp = self->audio_clock_.map(timestamp, arrival_ns);
    int log_index = self->audio_callback_logs_.fetch_add(1);
    if (log_index < 8) {
        lunar::diagnosticLog("webrtc", "audio callback begin len=%zu has_cb=%s",
                             size,
                             self->callbacks_.on_audio_frame ? "true" : "false");
    }
    if (self->callbacks_.on_audio_frame) {
        self->callbacks_.on_audio_frame(data, size, sequence, mapped_timestamp);
    }
    if (log_index < 8) {
        lunar::diagnosticLog("webrtc", "audio callback done");
    }
}

uint64_t PeerManager::recoverRtpClockAnchorArrivalNs(const char* track) const {
    const uint64_t callback_ns = elapsedNs(media_clock_start_);
    PeerConnectionMediaStats stats = {};
    peer_connection_get_media_stats(pc_, &stats);
    const uint64_t arrival_ns = recoverQueuedRtpArrivalNs(
        callback_ns, stats.rtp_queue_oldest_age_ms);
    lunar::diagnosticLog(
        "webrtc-clock",
        "%s anchor callback_ns=%llu queue_age_ms=%u arrival_ns=%llu depth=%u",
        track ? track : "unknown",
        static_cast<unsigned long long>(callback_ns),
        stats.rtp_queue_oldest_age_ms,
        static_cast<unsigned long long>(arrival_ns),
        stats.rtp_queue_depth);
    return arrival_ns;
}

void PeerManager::onDataChannelMessage(char* msg, size_t len, void* userdata, uint16_t sid) {
    auto* self = static_cast<PeerManager*>(userdata);
    char* label = peer_connection_lookup_sid_label(self->pc_, sid);
    std::string label_str = label ? label : "unknown";

    if (label_str == "input" && len >= 2) {
        auto* data = reinterpret_cast<uint8_t*>(msg);
        uint16_t report_type = data[0] | (static_cast<uint16_t>(data[1]) << 8);

        if (report_type & 0x80) {  // Vibration flag
            XboxVibrationCommand command;
            if (parseXboxVibrationPacket(data, len, command)) {
                if (self->callbacks_.on_rumble) {
                    self->callbacks_.on_rumble(command.gamepad_index,
                                               command.left_motor,
                                               command.right_motor,
                                               command.left_trigger,
                                               command.right_trigger,
                                               command.duration_ms,
                                               command.delay_ms,
                                               command.repeat);
                }
            } else if (self->rumble_parse_logs_.fetch_add(1) < 8) {
                lunar::diagnosticLog("webrtc",
                                     "invalid input vibration report type=0x%04x len=%zu",
                                     report_type,
                                     len);
            }
            return;
        }
    }

    if (self->callbacks_.on_datachannel_message) {
        self->callbacks_.on_datachannel_message(label_str,
            reinterpret_cast<const uint8_t*>(msg), len);
    }
}

// =============================================================================
// PeerManager
// =============================================================================
PeerManager::PeerManager() = default;

PeerManager::~PeerManager() {
    disconnect();
}

bool PeerManager::initialize() {
    if (initialized_) disconnect();
    if (peer_init() != 0) {
        lunar::diagnosticLog("webrtc", "peer_init failed");
        return false;
    }
    initialized_ = true;
    local_candidates_.clear();
    connected_ = false;
    data_channels_created_ = false;
    successful_ice_server_url_.clear();
    video_callback_logs_ = 0;
    crash_probe_access_units_ = 0;
    audio_callback_logs_ = 0;
    process_event_logs_ = 0;
    rumble_parse_logs_ = 0;
    nack_logs_ = 0;
    last_pump_socket_receive_us_total_ = 0;
    last_pump_receive_loop_us_total_ = 0;
    last_pump_rtp_drain_us_total_ = 0;
    last_pump_socket_packets_total_ = 0;
    last_pump_rtp_packets_decoded_total_ = 0;
    max_pump_phase_total_us_ = 0;
    slow_pump_count_ = 0;
    last_pump_phase_log_ = {};
    resetDataChannelHealth();
    outbound_drop_events_ = 0;
    next_input_sequence_ = 0;
    media_clock_start_ = std::chrono::steady_clock::now();
    video_clock_.reset();
    audio_clock_.reset();
    video_jitter_.reset();
    invalidateMediaStatsCache();
    setVideoJitterMode(video_jitter_mode_);
    clearOutboundCommands();

    PeerConfiguration config = {};
    config.video_codec = CODEC_H264;
    config.audio_codec = CODEC_OPUS;
    config.datachannel = DATA_CHANNEL_STRING;
    config.raw_video_rtp = 1;

    // Keep XStreaming's complete fallback list. A previously successful
    // server is only moved to the front when it is still in that list.
    size_t next_server = 0;
    const auto preferred = std::find_if(
        kXStreamingIceServers.begin(),
        kXStreamingIceServers.end(),
        [this](const char* url) { return preferred_ice_server_url_ == url; });
    if (preferred != kXStreamingIceServers.end()) {
        config.ice_servers[next_server++].urls = *preferred;
    }
    for (const char* url : kXStreamingIceServers) {
        if (preferred != kXStreamingIceServers.end() && url == *preferred) {
            continue;
        }
        config.ice_servers[next_server++].urls = url;
    }

    config.onvideotrack = onVideoTrack;
    config.onaudiotrack = onAudioTrack;
    config.user_data = this;

    pc_ = peer_connection_create(&config);
    if (!pc_) {
        fprintf(stderr, "[webrtc] peer_connection_create failed\n");
        lunar::diagnosticLog("webrtc", "peer_connection_create failed");
        peer_deinit();
        initialized_ = false;
        return false;
    }

    // Register ICE and state callbacks (userdata comes from PeerConfiguration)
    peer_connection_onicecandidate(pc_, onIceCandidate);
    peer_connection_oniceconnectionstatechange(pc_, onIceStateChange);

    // Register data channel callback
    peer_connection_ondatachannel(pc_, onDataChannelMessage, nullptr, nullptr);

    lunar::diagnosticLog("webrtc", "PeerManager initialized");
    return true;
}

void PeerManager::setCallbacks(const PeerCallbacks& callbacks) {
    video_callback_logs_ = 0;
    crash_probe_access_units_ = 0;
    audio_callback_logs_ = 0;
    process_event_logs_ = 0;
    rumble_parse_logs_ = 0;
    nack_logs_ = 0;
    callbacks_ = callbacks;
}

void PeerManager::setPreferredIceServerUrl(const std::string& url) {
    preferred_ice_server_url_ = url;
}

std::string PeerManager::successfulIceServerUrl() const {
    return successful_ice_server_url_;
}

std::string PeerManager::createOffer() {
    if (!pc_) return "";
    const char* sdp = peer_connection_create_offer(pc_);
    const char* successful_url =
        peer_connection_get_successful_ice_server_url(pc_);
    if (successful_url) {
        successful_ice_server_url_ = successful_url;
        preferred_ice_server_url_ = successful_url;
        lunar::diagnosticLog("webrtc",
                             "ICE server succeeded url=%s",
                             successful_url);
    }
    return sdp ? sdp : "";
}

void PeerManager::setRemoteAnswer(const std::string& sdp) {
    if (!pc_) return;
    lunar::diagnosticLog("webrtc", "set remote answer len=%zu", sdp.size());
    logRemoteMediaDescription(sdp);
    peer_connection_set_remote_description(pc_, sdp.c_str(), SDP_TYPE_ANSWER);
    lunar::diagnosticLog("webrtc", "set remote answer returned");
}

std::vector<IceCandidate> PeerManager::getLocalCandidates() {
    return local_candidates_;
}

void PeerManager::addIceCandidate(const std::string& candidate_sdp) {
    if (!pc_) return;
    for (const auto& candidate : extractCandidateLines(candidate_sdp)) {
        lunar::diagnosticLog("webrtc", "add remote ICE candidate: %s", candidate.c_str());
        peer_connection_add_ice_candidate(pc_,
            const_cast<char*>(candidate.c_str()));
    }
}

bool PeerManager::createDataChannels() {
    if (!pc_) return false;
    if (data_channels_created_) return true;
    if (!peer_connection_is_datachannel_connected(pc_)) return false;

    // Match XStreaming's creation order. As the DTLS client, locally initiated
    // WebRTC data channels use even SIDs (RFC 8832 section 6).
    for (const auto& ch : kXStreamingDataChannels) {
        uint16_t existing_sid = 0;
        if (peer_connection_lookup_sid(pc_, ch.label, &existing_sid) == 0) {
            continue;
        }
        const DecpChannelType channel_type =
            ch.max_retransmits == 0
                ? (ch.ordered ? DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT
                              : DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED)
                : (ch.ordered ? DATA_CHANNEL_RELIABLE
                              : DATA_CHANNEL_RELIABLE_UNORDERED);
        const uint32_t reliability = ch.max_retransmits < 0
            ? 0
            : static_cast<uint32_t>(ch.max_retransmits);
        int ret = peer_connection_create_datachannel_sid(pc_,
            channel_type, 0, reliability,
            const_cast<char*>(ch.label),
            const_cast<char*>(ch.protocol),
            ch.sid);
        if (ret < 0) {
            fprintf(stderr, "[webrtc] Failed to create datachannel %s (sid=%d): %d\n",
                    ch.label, ch.sid, ret);
            lunar::diagnosticLog("webrtc", "create datachannel failed label=%s sid=%u ret=%d",
                                 ch.label, ch.sid, ret);
            return false;
        }
        lunar::diagnosticLog("webrtc", "create datachannel label=%s sid=%u", ch.label, ch.sid);
    }
    data_channels_created_ = true;
    lunar::diagnosticLog("webrtc", "datachannels created");
    return true;
}

bool PeerManager::enqueueData(OutboundType type,
                              const uint8_t* data,
                              size_t len,
                              bool replace_existing,
                              std::chrono::milliseconds ttl) {
    if (!data || len == 0 || len > kMaxOutboundPayloadBytes ||
        !connected_.load() || data_channel_failed_.load()) {
        if (data && len > kMaxOutboundPayloadBytes) {
            logOutboundDrop("payload_too_large", type,
                            static_cast<int>(std::min<size_t>(len, INT32_MAX)));
        }
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        const auto enqueued_at = std::chrono::steady_clock::now();
#endif
        if (replace_existing) {
            for (auto& command : outbound_commands_) {
                if (command.type == type) {
                    command.payload.assign(data, data + len);
                    command.id = next_outbound_command_id_++;
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
                    command.enqueued_at = enqueued_at;
                    latency_input_enqueued_.fetch_add(
                        1, std::memory_order_relaxed);
                    if (type == OutboundType::InputLatest) {
                        latency_input_replaced_.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    recordOutboundQueueDepthLocked();
#endif
                    command.attempts = 0;
                    command.first_attempt_at = {};
                    command.expires_at = {};
                    return true;
                }
            }
        }
        if (outbound_commands_.size() >= kMaxOutboundCommands) {
            logOutboundDrop("queue_full", type);
            return false;
        }
        OutboundCommand command;
        command.type = type;
        command.payload.assign(data, data + len);
        command.id = next_outbound_command_id_++;
        if (ttl.count() > 0) {
            command.expires_at = std::chrono::steady_clock::now() + ttl;
        }
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        command.enqueued_at = enqueued_at;
#endif
        outbound_commands_.push_back(std::move(command));
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        if (type == OutboundType::InputReliable ||
            type == OutboundType::InputTransition ||
            type == OutboundType::InputLatest) {
            latency_input_enqueued_.fetch_add(1, std::memory_order_relaxed);
        }
        recordOutboundQueueDepthLocked();
#endif
        return true;
    } catch (...) {
        return false;
    }
}

bool PeerManager::enqueueSimple(OutboundCommand command, bool high_priority) {
    try {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        if (command.type == OutboundType::Pli) {
            for (const auto& queued : outbound_commands_) {
                if (queued.type == OutboundType::Pli) return true;
            }
        } else if (command.type == OutboundType::ReceiverFeedback) {
            for (auto& queued : outbound_commands_) {
                if (queued.type == OutboundType::ReceiverFeedback) {
                    command.id = next_outbound_command_id_++;
                    queued = std::move(command);
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
                    recordOutboundQueueDepthLocked();
#endif
                    return true;
                }
            }
        }
        if (outbound_commands_.size() >= kMaxOutboundCommands) {
            if (!high_priority) {
                logOutboundDrop("queue_full", command.type);
                return false;
            }
            auto discard = std::find_if(
                outbound_commands_.begin(),
                outbound_commands_.end(),
                [](const OutboundCommand& queued) {
                    return queued.type == OutboundType::InputLatest ||
                           queued.type == OutboundType::ReceiverFeedback ||
                           queued.type == OutboundType::Nack;
                });
            if (discard == outbound_commands_.end()) {
                logOutboundDrop("queue_full_reliable", command.type);
                return false;
            }
            logOutboundDrop("priority_eviction", discard->type);
            outbound_commands_.erase(discard);
        }
        command.id = next_outbound_command_id_++;
        if (command.type == OutboundType::Pli) {
            // SRTCP recovery must not wait behind a backpressured SCTP command.
            outbound_commands_.push_front(std::move(command));
        } else if (high_priority) {
            auto priority_position = std::find_if(
                outbound_commands_.begin(),
                outbound_commands_.end(),
                [](const OutboundCommand& queued) {
                    return queued.type == OutboundType::InputLatest ||
                           queued.type == OutboundType::ReceiverFeedback ||
                           queued.type == OutboundType::Nack;
                });
            outbound_commands_.insert(priority_position, std::move(command));
        } else {
            outbound_commands_.push_back(std::move(command));
        }
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        recordOutboundQueueDepthLocked();
#endif
        return true;
    } catch (...) {
        return false;
    }
}

bool PeerManager::enqueueNack(uint16_t pid, uint16_t blp) {
    if (!connected_.load()) return false;
    try {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        for (const auto& command : outbound_commands_) {
            if (command.type == OutboundType::Nack &&
                command.pid == pid && command.blp == blp) {
                return true;
            }
        }
        if (outbound_commands_.size() >= kMaxOutboundCommands) {
            auto discard = std::find_if(
                outbound_commands_.begin(), outbound_commands_.end(),
                [](const OutboundCommand& queued) {
                    return queued.type == OutboundType::InputLatest ||
                           queued.type == OutboundType::ReceiverFeedback;
                });
            if (discard == outbound_commands_.end()) {
                logOutboundDrop("queue_full", OutboundType::Nack);
                return false;
            }
            logOutboundDrop("nack_priority_eviction", discard->type);
            outbound_commands_.erase(discard);
        }
        OutboundCommand command;
        command.type = OutboundType::Nack;
        command.pid = pid;
        command.blp = blp;
        command.id = next_outbound_command_id_++;
        command.expires_at = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(250);
        const auto position = std::find_if(
            outbound_commands_.begin(), outbound_commands_.end(),
            [](const OutboundCommand& queued) {
                return queued.type != OutboundType::Pli &&
                       queued.type != OutboundType::Nack;
            });
        outbound_commands_.insert(position, std::move(command));
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        recordOutboundQueueDepthLocked();
#endif
        return true;
    } catch (...) {
        return false;
    }
}

bool PeerManager::sendInputData(const uint8_t* data, size_t len) {
    return enqueueData(OutboundType::InputReliable, data, len, false);
}

bool PeerManager::sendTransitionInputData(const uint8_t* data, size_t len) {
    return enqueueData(OutboundType::InputTransition, data, len, false,
                       kInputTransitionTtl);
}

bool PeerManager::sendLatestInputData(const uint8_t* data, size_t len) {
    return enqueueData(OutboundType::InputLatest, data, len, true);
}

bool PeerManager::sendControlData(const uint8_t* data, size_t len) {
    return enqueueData(OutboundType::Control, data, len, false);
}

bool PeerManager::sendMessageData(const uint8_t* data, size_t len) {
    return enqueueData(OutboundType::Message, data, len, false);
}

bool PeerManager::requestVideoKeyframe() {
    if (!connected_.load()) return false;
    OutboundCommand command;
    command.type = OutboundType::Pli;
    return enqueueSimple(std::move(command), true);
}

bool PeerManager::sendReceiverFeedback(uint32_t bitrate_bps) {
    if (!connected_.load() || bitrate_bps == 0) return false;
    const auto report = video_jitter_.receiverReport();
    OutboundCommand command;
    command.type = OutboundType::ReceiverFeedback;
    command.fraction_lost = report.fraction_lost;
    command.cumulative_lost = report.cumulative_lost;
    command.highest_sequence = report.highest_sequence;
    command.bitrate_bps = bitrate_bps;
    return enqueueSimple(std::move(command), false);
}

bool PeerManager::isReliableCommand(OutboundType type) {
    return type == OutboundType::InputReliable ||
           type == OutboundType::Control ||
           type == OutboundType::Message;
}

bool PeerManager::isSctpCommand(OutboundType type) {
    return isReliableCommand(type) ||
           type == OutboundType::InputTransition ||
           type == OutboundType::InputLatest;
}

bool PeerManager::isRecoverableSctpSendError(
    int result,
    bool data_channel_connected) {
    if (result >= 0) return false;
    if (peer_connection_is_transient_send_error(result)) return true;
    // Legacy libpeer returns a generic -1 for UDP send failures. If SCTP still
    // reports connected, that result is ambiguous rather than proof that the
    // association died. Latest-state input can be dropped and repaired by the
    // next 8 ms snapshot; reliable commands retain their bounded retry budget.
    return result == -1 && data_channel_connected;
}

int PeerManager::outboundPriority(OutboundType type) {
    switch (type) {
        case OutboundType::Pli: return 0;
        case OutboundType::Nack: return 1;
        case OutboundType::ReceiverFeedback: return 2;
        // Realtime input stays ahead of startup/control retries and is always
        // replaced with the newest complete state before it is sent.
        case OutboundType::InputTransition: return 3;
        case OutboundType::InputLatest: return 4;
        case OutboundType::InputReliable: return 5;
        case OutboundType::Control:
        case OutboundType::Message: return 6;
    }
    return 6;
}

bool PeerManager::selectOutboundCommand(OutboundCommand& command,
                                        bool allow_sctp,
                                        bool realtime_input_only) const {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    auto selected = outbound_commands_.end();
    int selected_priority = 7;
    for (auto it = outbound_commands_.begin();
         it != outbound_commands_.end(); ++it) {
        if (realtime_input_only &&
            it->type != OutboundType::InputTransition &&
            it->type != OutboundType::InputLatest) {
            continue;
        }
        if (!allow_sctp && isSctpCommand(it->type)) continue;
        const int priority = outboundPriority(it->type);
        if (priority < selected_priority) {
            selected = it;
            selected_priority = priority;
        }
    }
    if (selected == outbound_commands_.end()) return false;
    command = *selected;
    return true;
}

bool PeerManager::hasPendingReliableData() const {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    return std::any_of(outbound_commands_.begin(), outbound_commands_.end(),
                       [](const OutboundCommand& command) {
                           return isReliableCommand(command.type);
                       });
}

bool PeerManager::consumeDataChannelFailure() {
    return data_channel_failure_event_.exchange(false);
}

void PeerManager::logOutboundDrop(const char* reason,
                                  OutboundType type,
                                  int result) noexcept {
    const uint32_t count = outbound_drop_events_.fetch_add(1) + 1;
    if (count <= 32 || count % 64 == 0) {
        lunar::dropDiagnosticLog(
            "webrtc-outbound",
            "reason=%s type=%d result=%d event_count=%u",
            reason ? reason : "unknown",
            static_cast<int>(type),
            result,
            count);
    }
}

int PeerManager::sendOutboundCommand(const OutboundCommand& command) {
    if (!pc_ || !connected_.load()) return -1;
    if ((command.type == OutboundType::InputReliable ||
         command.type == OutboundType::InputTransition ||
         command.type == OutboundType::InputLatest ||
         command.type == OutboundType::Control ||
         command.type == OutboundType::Message) &&
        !data_channels_created_ && !createDataChannels()) {
        return -1;
    }
    switch (command.type) {
        case OutboundType::InputReliable:
        case OutboundType::InputTransition:
        case OutboundType::InputLatest:
            return sendInputCommand(command);
        case OutboundType::Control:
            return peer_connection_datachannel_send_sid(
                pc_,
                reinterpret_cast<char*>(const_cast<uint8_t*>(command.payload.data())),
                command.payload.size(),
                xstreamingDataChannelSid("control"));
        case OutboundType::Message:
            return peer_connection_datachannel_send_sid(
                pc_,
                reinterpret_cast<char*>(const_cast<uint8_t*>(command.payload.data())),
                command.payload.size(),
                xstreamingDataChannelSid("message"));
        case OutboundType::Nack:
            return peer_connection_send_nack(pc_, command.pid, command.blp);
        case OutboundType::Pli:
            return peer_connection_send_rtcp_pil(pc_, 0);
        case OutboundType::ReceiverFeedback:
            return peer_connection_send_receiver_feedback_stats(
                pc_, command.fraction_lost, command.cumulative_lost,
                command.highest_sequence, 0, command.bitrate_bps);
    }
    return -1;
}

bool PeerManager::prepareSequencedInputPayload(
    const OutboundCommand& command,
    std::vector<uint8_t>& packet) const {
    if (command.payload.size() < 6 ||
        command.payload.size() > kMaxOutboundPayloadBytes) {
        return false;
    }
    packet = command.payload;
    return input::XInputEncoder::stampSequence(
        packet.data(), packet.size(), next_input_sequence_);
}

void PeerManager::commitSequencedInputResult(int result) {
    if (result >= 0) {
        ++next_input_sequence_;
    }
}

int PeerManager::sendInputCommand(const OutboundCommand& command) {
    if (!pc_) return -1;
    std::vector<uint8_t> packet;
    if (!prepareSequencedInputPayload(command, packet)) return -1;

    const int result = peer_connection_datachannel_send_sid_binary(
        pc_,
        reinterpret_cast<char*>(packet.data()),
        packet.size(),
        xstreamingDataChannelSid("input"));
    commitSequencedInputResult(result);
    return result;
}

void PeerManager::drainOutboundCommands(
    std::chrono::steady_clock::time_point deadline,
    bool realtime_input_only) {
    const size_t max_commands = realtime_input_only ? 1 : 8;
    bool allow_sctp = true;
    for (size_t sent = 0; sent < max_commands; ++sent) {
        OutboundCommand command;
        try {
            if (!selectOutboundCommand(command, allow_sctp,
                                       realtime_input_only)) {
                return;
            }
        } catch (...) {
            logOutboundDrop("snapshot_failed", OutboundType::InputReliable);
            return;
        }
        if (command.expires_at.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() >= command.expires_at) {
            completeOutboundCommand(command, 0, false);
            logOutboundDrop("expired", command.type);
            continue;
        }
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        const bool is_input = command.type == OutboundType::InputReliable ||
                              command.type == OutboundType::InputTransition ||
                              command.type == OutboundType::InputLatest;
        const auto send_started = std::chrono::steady_clock::now();
        if (is_input && command.enqueued_at.time_since_epoch().count() != 0) {
            const uint64_t queue_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    send_started - command.enqueued_at).count());
            latency_input_queue_total_us_.fetch_add(
                queue_us, std::memory_order_relaxed);
            latency_input_queue_samples_.fetch_add(
                1, std::memory_order_relaxed);
            recordLatencyMaximum(latency_input_queue_max_us_, queue_us);
        }
#endif
        const int result = sendOutboundCommand(command);
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        if (is_input) {
            const uint64_t send_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - send_started).count());
            latency_input_send_total_us_.fetch_add(
                send_us, std::memory_order_relaxed);
            latency_input_send_samples_.fetch_add(
                1, std::memory_order_relaxed);
            recordLatencyMaximum(latency_input_send_max_us_, send_us);
            if (result >= 0) {
                latency_input_sent_.fetch_add(1, std::memory_order_relaxed);
            } else {
                latency_input_send_failures_.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
#endif
        const bool data_channel_connected =
            result < 0 && isSctpCommand(command.type) && pc_ &&
            peer_connection_is_datachannel_connected(pc_);
        if (completeOutboundCommand(command, result,
                                    data_channel_connected)) {
            if (!isSctpCommand(command.type)) return;
            allow_sctp = false;
            continue;
        }
        if (sent > 0 && std::chrono::steady_clock::now() >= deadline) return;
    }
}

bool PeerManager::completeOutboundCommand(const OutboundCommand& command,
                                          int result,
                                          bool data_channel_connected) {
    const auto now = std::chrono::steady_clock::now();
    const bool recoverable =
        isRecoverableSctpSendError(result, data_channel_connected);
    const auto first_attempt =
        command.first_attempt_at.time_since_epoch().count() == 0
            ? now
            : command.first_attempt_at;
    const bool reliable_budget_available =
        command.attempts + 1 < kMaxReliableSendAttempts &&
        now - first_attempt < kMaxSctpRecoverableFailureAge;
    const bool keep_for_retry =
        (recoverable && isReliableCommand(command.type) &&
         reliable_budget_available) ||
        (result < 0 && command.type == OutboundType::Nack &&
         command.attempts == 0) ||
        (result < 0 && command.type == OutboundType::Pli &&
         command.attempts + 1 < kMaxPliSendAttempts);
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        auto queued = std::find_if(
            outbound_commands_.begin(), outbound_commands_.end(),
            [&command](const OutboundCommand& candidate) {
                return candidate.id == command.id;
            });
        if (queued != outbound_commands_.end() && !keep_for_retry) {
            outbound_commands_.erase(queued);
        } else if (queued != outbound_commands_.end() &&
                   (isReliableCommand(command.type) ||
                    command.type == OutboundType::Nack ||
                    command.type == OutboundType::Pli)) {
            queued->attempts++;
            if (queued->first_attempt_at.time_since_epoch().count() == 0) {
                queued->first_attempt_at = now;
            }
        }
    }
    if (isSctpCommand(command.type)) {
        observeSctpSendResult(command.type, result, command.attempts + 1,
                              data_channel_connected);
    }
    if (result >= 0) return false;
    if (keep_for_retry) {
        logOutboundDrop("send_retry", command.type, result);
        return true;
    }
    logOutboundDrop(recoverable ? "recoverable_drop" : "send_failed",
                    command.type, result);
    if (isReliableCommand(command.type) && recoverable &&
        !reliable_budget_available) {
        markDataChannelFailed("sctp-reliable-retry-exhausted",
                              command.type, result, command.attempts + 1,
                              data_channel_connected);
    }
    return false;
}

void PeerManager::observeSctpSendResult(OutboundType type,
                                        int result,
                                        uint32_t attempts,
                                        bool data_channel_connected) {
    if (result >= 0) {
        consecutive_sctp_send_failures_ = 0;
        first_sctp_send_failure_ = {};
        return;
    }
    const bool recoverable =
        isRecoverableSctpSendError(result, data_channel_connected);
    if (!recoverable) {
        markDataChannelFailed("sctp-fatal-send-failure",
                              type, result, attempts,
                              data_channel_connected);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (consecutive_sctp_send_failures_++ == 0) {
        first_sctp_send_failure_ = now;
    }
    if (consecutive_sctp_send_failures_ >=
            kMaxConsecutiveSctpSendFailures ||
        now - first_sctp_send_failure_ >= kMaxSctpRecoverableFailureAge) {
        const char* reason = peer_connection_is_transient_send_error(result)
            ? "sctp-transient-budget-exhausted"
            : "sctp-ambiguous-budget-exhausted";
        markDataChannelFailed(reason, type, result, attempts,
                              data_channel_connected);
    }
}

void PeerManager::markDataChannelFailed(const char* reason,
                                        OutboundType type,
                                        int result,
                                        uint32_t attempts,
                                        bool data_channel_connected) {
    if (data_channel_failed_.exchange(true)) return;
    data_channel_failure_event_ = true;
    const auto age_ms = first_sctp_send_failure_.time_since_epoch().count() == 0
        ? 0
        : std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - first_sctp_send_failure_).count();
    lunar::persistentEventLog(
        "webrtc-outbound",
        "%s type=%d result=%d transient=%s recoverable=%s "
        "data_channel_connected=%s socket_errno=%d consecutive=%u "
        "age_ms=%lld attempts=%u action=reconnect",
        reason ? reason : "sctp-send-failure",
        static_cast<int>(type), result,
        peer_connection_is_transient_send_error(result) ? "true" : "false",
        isRecoverableSctpSendError(result, data_channel_connected)
            ? "true" : "false",
        data_channel_connected ? "true" : "false",
        pc_ ? peer_connection_get_last_send_error(pc_) : 0,
        consecutive_sctp_send_failures_,
        static_cast<long long>(age_ms), attempts);
}

void PeerManager::resetDataChannelHealth() {
    data_channel_failed_ = false;
    data_channel_failure_event_ = false;
    consecutive_sctp_send_failures_ = 0;
    first_sctp_send_failure_ = {};
}

void PeerManager::clearOutboundCommands() {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    outbound_commands_.clear();
    next_outbound_command_id_ = 1;
}

#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
void PeerManager::recordOutboundQueueDepthLocked() noexcept {
    const uint32_t depth = static_cast<uint32_t>(std::min<size_t>(
        UINT32_MAX, outbound_commands_.size()));
    recordLatencyMaximum(latency_outbound_queue_high_watermark_, depth);
}
#endif

bool PeerManager::isConnected() const {
    return connected_ && !data_channel_failed_;
}

bool PeerManager::isDataChannelReady() const {
    return pc_ && !data_channel_failed_ &&
           peer_connection_is_datachannel_connected(pc_);
}

void PeerManager::setMediaEnabled(bool enabled) {
    if (pc_) {
        lunar::diagnosticLog("webrtc", "set media enabled=%s", enabled ? "true" : "false");
        peer_connection_set_media_enabled(pc_, enabled ? 1 : 0);
    }
}

void PeerManager::setVideoJitterMode(VideoJitterMode mode) {
    video_jitter_mode_ = mode;
    network_path_estimator_.reset(mode);
    network_path_estimate_ = network_path_estimator_.estimate();
    network_path_window_started_ = {};
    applyVideoJitterPolicy(network_path_estimate_);
}

void PeerManager::applyVideoJitterPolicy(const NetworkPathEstimate& path) {
    const auto policy = computeVideoJitterPolicy(video_jitter_mode_, path);
    video_jitter_.setHoldMs(policy.frame_hold_ms);
    video_jitter_.setMissingPacketHoldMs(policy.missing_packet_hold_ms);
    video_jitter_.setRecoveryHoldMs(policy.recovery_hold_ms);
    video_jitter_.setNetworkRttMs(path.smoothed_rtt_ms > 0
        ? path.smoothed_rtt_ms : path.raw_rtt_ms);
    video_jitter_.setHeadBlockedPolicy(policy.max_head_blocked_frames,
                                       policy.head_blocked_hold_ms);
}

void PeerManager::updateNetworkPathEstimate(const PeerMediaStats& stats) {
    const auto now = std::chrono::steady_clock::now();
    const bool first_sample =
        network_path_window_started_.time_since_epoch().count() == 0;
    if (!first_sample &&
        now - network_path_window_started_ < std::chrono::seconds(1)) {
        return;
    }
    const uint32_t interval_ms = first_sample ? 1000u :
        static_cast<uint32_t>(std::max<int64_t>(1,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - network_path_window_started_).count()));
    network_path_window_started_ = now;

    NetworkPathSample sample;
    sample.video_rtp_packets = stats.video_rtp_packets;
    sample.video_payload_bytes = stats.video_rtp_payload_bytes;
    sample.video_missing_detected = stats.video_rtp_missing_packets_detected;
    sample.video_missing_recovered = stats.video_rtp_missing_packets_recovered;
    sample.video_missing_unrecovered =
        stats.video_rtp_missing_packets_unrecovered;
    sample.rtp_queue_drops = stats.rtp_queue_drops;
    sample.rtp_queue_depth = stats.rtp_queue_depth;
    sample.rtt_ms = stats.ice_rtt_ms;
    sample.interval_ms = interval_ms;
    network_path_estimate_ = network_path_estimator_.observe(sample);
    applyVideoJitterPolicy(network_path_estimate_);

    if (!network_path_estimate_.valid) return;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    const auto jitter_policy = computeVideoJitterPolicy(
        video_jitter_mode_, network_path_estimate_);
    lunar::dropDiagnosticLog(
        "xbox-net-quality",
        "mode=%s quality=%s observed=%s rtt_ms=%u srtt_ms=%u baseline_ms=%u "
        "inflation_ms=%u packets=%u detected=%u recovered=%u unrecovered=%u "
        "detected_loss_ppm=%llu unrecovered_loss_ppm=%llu bitrate_kbps=%u "
        "queue_depth=%u queue_drops=%u frame_hold_ms=%llu "
        "missing_hold_ms=%llu recovery_hold_ms=%llu head_frames=%zu "
        "head_hold_ms=%llu",
        video_jitter_mode_ == VideoJitterMode::Cloud ? "cloud" : "home",
        networkPathQualityName(network_path_estimate_.quality),
        networkPathQualityName(network_path_estimate_.observed_quality),
        network_path_estimate_.raw_rtt_ms,
        network_path_estimate_.smoothed_rtt_ms,
        network_path_estimate_.baseline_rtt_ms,
        network_path_estimate_.rtt_inflation_ms,
        network_path_estimate_.window_packets,
        network_path_estimate_.detected_missing,
        network_path_estimate_.recovered_missing,
        network_path_estimate_.unrecovered_missing,
        static_cast<unsigned long long>(
            network_path_estimate_.detected_loss_ppm),
        static_cast<unsigned long long>(
            network_path_estimate_.unrecovered_loss_ppm),
        network_path_estimate_.received_bitrate_kbps,
        network_path_estimate_.queue_depth,
        network_path_estimate_.queue_drops,
        static_cast<unsigned long long>(jitter_policy.frame_hold_ms),
        static_cast<unsigned long long>(jitter_policy.missing_packet_hold_ms),
        static_cast<unsigned long long>(jitter_policy.recovery_hold_ms),
        jitter_policy.max_head_blocked_frames,
        static_cast<unsigned long long>(jitter_policy.head_blocked_hold_ms));
#endif
}

PeerConnectionMediaStats PeerManager::networkStatsSnapshot() const {
    std::lock_guard<std::mutex> lock(media_stats_mutex_);
    const auto now = std::chrono::steady_clock::now();
    const bool cache_fresh = media_stats_cache_valid_ &&
        now - media_stats_cache_at_ < kMediaStatsCacheInterval;
    if (!cache_fresh) {
        if (pc_) {
            peer_connection_get_media_stats(pc_, &media_stats_cache_);
        } else {
            media_stats_cache_ = {};
        }
        media_stats_cache_at_ = now;
        media_stats_cache_valid_ = true;
    }
    return media_stats_cache_;
}

void PeerManager::invalidateMediaStatsCache() {
    std::lock_guard<std::mutex> lock(media_stats_mutex_);
    media_stats_cache_ = {};
    media_stats_cache_at_ = {};
    media_stats_cache_valid_ = false;
}

PeerMediaStats PeerManager::getMediaStats() const {
    PeerMediaStats stats = {};
    static_cast<PeerConnectionMediaStats&>(stats) = networkStatsSnapshot();
    const auto video = video_jitter_.stats();
    stats.video_rtp_packets = video.packets;
    stats.video_rtp_payload_bytes = video.payload_bytes;
    stats.video_rtp_sequence_gaps = video.sequence_gaps;
    stats.video_rtp_missing_packets = video.missing_packets;
    stats.video_rtp_missing_packets_detected = video.missing_packets_detected;
    stats.video_rtp_missing_packets_recovered = video.missing_packets_recovered;
    stats.video_rtp_missing_packets_unrecovered =
        video.missing_packets_unrecovered;
    stats.video_h264_frames = video.frames;
    stats.video_h264_corrupt_frames = video.corrupt_frames;
    stats.video_h264_unsupported_nalus = video.unsupported_nalus;
    stats.video_h264_overflow_frames = video.overflow_frames;
    stats.video_h264_max_frame_bytes = video.max_frame_bytes;
    stats.video_rtp_highest_seq_ext = video.highest_sequence;
    // Recovery state drives Xbox PLI requests and must not depend on whether
    // diagnostic-only counters are compiled into the release build.
    stats.video_waiting_keyframe = video_jitter_.waitingForKeyframe();
    stats.video_rtp_nacks = video.nacks;
    stats.video_rtp_nack_retries = video.nack_retries;
    stats.video_rtp_resyncs = video.resyncs;
    stats.video_rtp_timestamp_discontinuities = video.timestamp_discontinuities;
    stats.video_rtp_last_gap_packets = video.last_gap_packets;
    stats.video_rtp_ssrc = video.ssrc;
    stats.video_rtp_ssrc_changes = video.ssrc_changes;
    const uint64_t media_now_ms = elapsedNs(media_clock_start_) / 1000000u;
    stats.video_rtp_arrival_age_ms = video.last_arrival_ms > 0 &&
            media_now_ms >= video.last_arrival_ms
        ? static_cast<uint32_t>(std::min<uint64_t>(
              UINT32_MAX, media_now_ms - video.last_arrival_ms))
        : 0;
    stats.video_rtp_last_arrival_gap_ms = static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX, video.last_arrival_gap_ms));
    stats.video_rtp_max_arrival_gap_ms = static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX, video.max_arrival_gap_ms));
    stats.video_jitter_buffered_packets = static_cast<uint32_t>(
        std::min<size_t>(UINT32_MAX, video.buffered_packets));
    stats.video_jitter_buffered_frames = static_cast<uint32_t>(
        std::min<size_t>(UINT32_MAX, video.buffered_frames));
    stats.video_jitter_buffered_bytes = static_cast<uint32_t>(
        std::min<size_t>(UINT32_MAX, video.buffered_bytes));
    stats.network_path = network_path_estimate_;
    return stats;
}

PeerLatencyWindow PeerManager::takeLatencyWindow() {
    PeerLatencyWindow window;
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
    window.pump_total_us = latency_pump_total_us_.exchange(
        0, std::memory_order_relaxed);
    window.pump_max_us = latency_pump_max_us_.exchange(
        0, std::memory_order_relaxed);
    window.pump_samples = latency_pump_samples_.exchange(
        0, std::memory_order_relaxed);
    window.pump_gap_total_us = latency_pump_gap_total_us_.exchange(
        0, std::memory_order_relaxed);
    window.pump_gap_max_us = latency_pump_gap_max_us_.exchange(
        0, std::memory_order_relaxed);
    window.pump_gap_samples = latency_pump_gap_samples_.exchange(
        0, std::memory_order_relaxed);
    window.peer_loop_total_us = latency_peer_loop_total_us_.exchange(
        0, std::memory_order_relaxed);
    window.peer_loop_max_us = latency_peer_loop_max_us_.exchange(
        0, std::memory_order_relaxed);
    window.socket_total_us = latency_socket_total_us_.exchange(
        0, std::memory_order_relaxed);
    window.socket_max_us = latency_socket_max_us_.exchange(
        0, std::memory_order_relaxed);
    window.receive_loop_total_us = latency_receive_loop_total_us_.exchange(
        0, std::memory_order_relaxed);
    window.receive_loop_max_us = latency_receive_loop_max_us_.exchange(
        0, std::memory_order_relaxed);
    window.rtp_drain_total_us = latency_rtp_drain_total_us_.exchange(
        0, std::memory_order_relaxed);
    window.rtp_drain_max_us = latency_rtp_drain_max_us_.exchange(
        0, std::memory_order_relaxed);
    window.outbound_total_us = latency_outbound_total_us_.exchange(
        0, std::memory_order_relaxed);
    window.outbound_max_us = latency_outbound_max_us_.exchange(
        0, std::memory_order_relaxed);
    window.other_total_us = latency_other_total_us_.exchange(
        0, std::memory_order_relaxed);
    window.other_max_us = latency_other_max_us_.exchange(
        0, std::memory_order_relaxed);
    window.socket_packets = latency_socket_packets_.exchange(
        0, std::memory_order_relaxed);
    window.rtp_packets_decoded = latency_rtp_packets_decoded_.exchange(
        0, std::memory_order_relaxed);
    window.input_enqueued = latency_input_enqueued_.exchange(
        0, std::memory_order_relaxed);
    window.input_replaced = latency_input_replaced_.exchange(
        0, std::memory_order_relaxed);
    window.input_sent = latency_input_sent_.exchange(
        0, std::memory_order_relaxed);
    window.input_send_failures = latency_input_send_failures_.exchange(
        0, std::memory_order_relaxed);
    window.input_queue_total_us = latency_input_queue_total_us_.exchange(
        0, std::memory_order_relaxed);
    window.input_queue_max_us = latency_input_queue_max_us_.exchange(
        0, std::memory_order_relaxed);
    window.input_queue_samples = latency_input_queue_samples_.exchange(
        0, std::memory_order_relaxed);
    window.input_send_total_us = latency_input_send_total_us_.exchange(
        0, std::memory_order_relaxed);
    window.input_send_max_us = latency_input_send_max_us_.exchange(
        0, std::memory_order_relaxed);
    window.input_send_samples = latency_input_send_samples_.exchange(
        0, std::memory_order_relaxed);
    window.outbound_queue_high_watermark =
        latency_outbound_queue_high_watermark_.exchange(
            0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        window.outbound_queue_depth = static_cast<uint32_t>(
            std::min<size_t>(UINT32_MAX, outbound_commands_.size()));
    }
    window.video_jitter = video_jitter_.takeLatencyWindow();
#endif
    return window;
}

void PeerManager::processEvents() {
    if (pc_) {
        const auto pump_started = std::chrono::steady_clock::now();
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        if (latency_last_pump_started_.time_since_epoch().count() != 0) {
            const uint64_t gap_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    pump_started - latency_last_pump_started_).count());
            latency_pump_gap_total_us_.fetch_add(
                gap_us, std::memory_order_relaxed);
            latency_pump_gap_samples_.fetch_add(1, std::memory_order_relaxed);
            recordLatencyMaximum(latency_pump_gap_max_us_, gap_us);
        }
        latency_last_pump_started_ = pump_started;
#endif
        int log_index = process_event_logs_.fetch_add(1);
        if (log_index < 16) {
            lunar::diagnosticLog("webrtc", "processEvents begin index=%d", log_index);
        }
        // The owner loop has just published its newest controller snapshot.
        // Send only that replaceable state before inbound socket/RTP work so a
        // busy peer loop cannot add its full duration to input latency. Normal
        // recovery and reliable-control priority is preserved by the full
        // drain after peer_connection_loop().
        constexpr bool realtime_input_only = true;
        const auto realtime_outbound_started =
            std::chrono::steady_clock::now();
        drainOutboundCommands(realtime_outbound_started +
                                  std::chrono::microseconds(500),
                              realtime_input_only);
        const auto realtime_outbound_finished =
            std::chrono::steady_clock::now();
        constexpr int kMaxDrainPasses = 8;
        constexpr auto kMaxDrainTime = std::chrono::milliseconds(3);
        const auto drain_start = std::chrono::steady_clock::now();
        int drain_passes = 0;
        int drain_work = 0;
        const auto peer_loop_started = std::chrono::steady_clock::now();
        while (drain_passes < kMaxDrainPasses) {
            const int work = peer_connection_loop(pc_);
            drain_work += work;
            drain_passes++;
            if (work <= 0 ||
                std::chrono::steady_clock::now() - drain_start >= kMaxDrainTime) {
                break;
            }
        }
        const auto peer_loop_finished = std::chrono::steady_clock::now();
        if (log_index < 16) {
            lunar::diagnosticLog("webrtc", "processEvents after peer loop index=%d passes=%d work=%d connected=%s data_created=%s data_ready=%s",
                                 log_index,
                                 drain_passes,
                                 drain_work,
                                 connected_.load() ? "true" : "false",
                                 data_channels_created_ ? "true" : "false",
                                 peer_connection_is_datachannel_connected(pc_) ? "true" : "false");
        }
        if (connected_ && !data_channels_created_ && peer_connection_is_datachannel_connected(pc_)) {
            lunar::diagnosticLog("webrtc", "datachannel connected, creating channels");
            createDataChannels();
        }
        const auto outbound_started = std::chrono::steady_clock::now();
        drainOutboundCommands(outbound_started + std::chrono::milliseconds(1));
        const auto outbound_finished = std::chrono::steady_clock::now();
        const PeerMediaStats network_stats = getMediaStats();
        updateNetworkPathEstimate(network_stats);

        const auto pump_finished = std::chrono::steady_clock::now();
        const auto elapsed_us = [](auto start, auto finish) -> uint64_t {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    finish - start).count());
        };
        const auto delta = [](uint64_t current, uint64_t previous) -> uint64_t {
            return current >= previous ? current - previous : current;
        };
        const uint64_t socket_us = delta(
            network_stats.pump_socket_receive_us_total,
            last_pump_socket_receive_us_total_);
        const uint64_t receive_loop_us = delta(
            network_stats.pump_receive_loop_us_total,
            last_pump_receive_loop_us_total_);
        const uint64_t rtp_drain_us = delta(
            network_stats.pump_rtp_drain_us_total,
            last_pump_rtp_drain_us_total_);
        const uint64_t socket_packets = delta(
            network_stats.pump_socket_packets_total,
            last_pump_socket_packets_total_);
        const uint64_t rtp_decoded = delta(
            network_stats.pump_rtp_packets_decoded_total,
            last_pump_rtp_packets_decoded_total_);
        last_pump_socket_receive_us_total_ =
            network_stats.pump_socket_receive_us_total;
        last_pump_receive_loop_us_total_ =
            network_stats.pump_receive_loop_us_total;
        last_pump_rtp_drain_us_total_ =
            network_stats.pump_rtp_drain_us_total;
        last_pump_socket_packets_total_ =
            network_stats.pump_socket_packets_total;
        last_pump_rtp_packets_decoded_total_ =
            network_stats.pump_rtp_packets_decoded_total;

        const uint64_t total_us = elapsed_us(pump_started, pump_finished);
        const uint64_t peer_loop_us = elapsed_us(peer_loop_started,
                                                 peer_loop_finished);
        const uint64_t outbound_us =
            elapsed_us(realtime_outbound_started,
                       realtime_outbound_finished) +
            elapsed_us(outbound_started, outbound_finished);
        const uint64_t accounted_us = receive_loop_us + rtp_drain_us + outbound_us;
        const uint64_t other_us = total_us > accounted_us
            ? total_us - accounted_us
            : 0;
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
        latency_pump_total_us_.fetch_add(total_us, std::memory_order_relaxed);
        latency_pump_samples_.fetch_add(1, std::memory_order_relaxed);
        recordLatencyMaximum(latency_pump_max_us_, total_us);
        latency_peer_loop_total_us_.fetch_add(peer_loop_us,
                                              std::memory_order_relaxed);
        recordLatencyMaximum(latency_peer_loop_max_us_, peer_loop_us);
        latency_socket_total_us_.fetch_add(socket_us,
                                           std::memory_order_relaxed);
        recordLatencyMaximum(latency_socket_max_us_, socket_us);
        latency_receive_loop_total_us_.fetch_add(receive_loop_us,
                                                 std::memory_order_relaxed);
        recordLatencyMaximum(latency_receive_loop_max_us_, receive_loop_us);
        latency_rtp_drain_total_us_.fetch_add(rtp_drain_us,
                                              std::memory_order_relaxed);
        recordLatencyMaximum(latency_rtp_drain_max_us_, rtp_drain_us);
        latency_outbound_total_us_.fetch_add(outbound_us,
                                             std::memory_order_relaxed);
        recordLatencyMaximum(latency_outbound_max_us_, outbound_us);
        latency_other_total_us_.fetch_add(other_us,
                                          std::memory_order_relaxed);
        recordLatencyMaximum(latency_other_max_us_, other_us);
        latency_socket_packets_.fetch_add(socket_packets,
                                          std::memory_order_relaxed);
        latency_rtp_packets_decoded_.fetch_add(rtp_decoded,
                                               std::memory_order_relaxed);
#endif
        constexpr uint64_t kSlowPumpThresholdUs = 10000;
        if (total_us >= kSlowPumpThresholdUs) {
            slow_pump_count_++;
            const bool new_max = total_us > max_pump_phase_total_us_;
            max_pump_phase_total_us_ = std::max(max_pump_phase_total_us_, total_us);
            const bool interval_elapsed =
                last_pump_phase_log_.time_since_epoch().count() == 0 ||
                pump_finished - last_pump_phase_log_ >= std::chrono::seconds(1);
            if (new_max || interval_elapsed) {
                lunar::dropDiagnosticLog(
                    "webrtc-pump",
                    "DEBUG-pump-phase total_us=%llu peer_loop_us=%llu "
                    "socket_us=%llu receive_loop_us=%llu rtp_drain_us=%llu "
                    "outbound_us=%llu other_us=%llu passes=%d work=%d "
                    "socket_packets=%llu rtp_decoded=%llu queue_depth=%u "
                    "queue_oldest_ms=%u slow_count=%llu max_total_us=%llu",
                    static_cast<unsigned long long>(total_us),
                    static_cast<unsigned long long>(peer_loop_us),
                    static_cast<unsigned long long>(socket_us),
                    static_cast<unsigned long long>(receive_loop_us),
                    static_cast<unsigned long long>(rtp_drain_us),
                    static_cast<unsigned long long>(outbound_us),
                    static_cast<unsigned long long>(other_us),
                    drain_passes,
                    drain_work,
                    static_cast<unsigned long long>(socket_packets),
                    static_cast<unsigned long long>(rtp_decoded),
                    network_stats.rtp_queue_depth,
                    network_stats.rtp_queue_oldest_age_ms,
                    static_cast<unsigned long long>(slow_pump_count_),
                    static_cast<unsigned long long>(max_pump_phase_total_us_));
                last_pump_phase_log_ = pump_finished;
            }
        }
        if (log_index < 16) {
            lunar::diagnosticLog("webrtc", "processEvents done index=%d", log_index);
        }
    }
}

void PeerManager::disconnect() {
    lunar::diagnosticLog("webrtc", "disconnect begin pc=%p initialized=%s",
                         static_cast<void*>(pc_),
                         initialized_ ? "true" : "false");
    connected_ = false;
    resetDataChannelHealth();
    clearOutboundCommands();
    if (pc_) {
        lunar::diagnosticLog("webrtc", "disconnect close begin");
        peer_connection_close(pc_);
        lunar::diagnosticLog("webrtc", "disconnect close done");
        lunar::diagnosticLog("webrtc", "disconnect destroy begin");
        peer_connection_destroy(pc_);
        lunar::diagnosticLog("webrtc", "disconnect destroy done");
        pc_ = nullptr;
    }
    data_channels_created_ = false;
    network_path_estimator_.reset(video_jitter_mode_);
    network_path_estimate_ = network_path_estimator_.estimate();
    network_path_window_started_ = {};
#if LUNARNX_LATENCY_DIAGNOSTIC_LOG
    latency_last_pump_started_ = {};
#endif
    invalidateMediaStatsCache();
    video_jitter_.reset();
    nack_logs_ = 0;
    if (initialized_) {
        lunar::diagnosticLog("webrtc", "disconnect peer_deinit begin");
        peer_deinit();
        lunar::diagnosticLog("webrtc", "disconnect peer_deinit done");
        initialized_ = false;
    }
    lunar::diagnosticLog("webrtc", "disconnect done");
}

} // namespace lunar::webrtc
