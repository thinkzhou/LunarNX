#include "peer_manager.h"
#include "xbox_input_feedback.h"
#include "xstreaming_data_channels.h"
#include "../diagnostics.h"
#include <cJSON.h>
#include <peer.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>

namespace lunar::webrtc {

namespace {

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
                const uint64_t mapped_timestamp =
                    self->video_clock_.map(frame_timestamp, arrival_ns);
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
    const uint64_t arrival_ns = elapsedNs(self->media_clock_start_);
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
    video_callback_logs_ = 0;
    crash_probe_access_units_ = 0;
    audio_callback_logs_ = 0;
    process_event_logs_ = 0;
    rumble_parse_logs_ = 0;
    nack_logs_ = 0;
    reliable_send_failed_ = false;
    outbound_drop_events_ = 0;
    media_clock_start_ = std::chrono::steady_clock::now();
    video_clock_.reset();
    audio_clock_.reset();
    video_jitter_.reset();
    clearOutboundCommands();

    PeerConfiguration config = {};
    config.video_codec = CODEC_H264;
    config.audio_codec = CODEC_OPUS;
    config.datachannel = DATA_CHANNEL_STRING;
    config.raw_video_rtp = 1;

    // Keep candidate gathering aligned with XStreaming's WebRTC configuration.
    config.ice_servers[0].urls = "stun:worldaz.relay.teams.microsoft.com:3478";
    config.ice_servers[1].urls = "stun:stun.l.google.com:19302";
    config.ice_servers[2].urls = "stun:stun1.l.google.com:19302";
    config.ice_servers[3].urls = "stun:relay1.expressturn.com";
    config.ice_servers[4].urls = "stun:relay2.expressturn.com";
    config.ice_servers[5].urls = "stun:stun.kinesisvideo.us-east-1.amazonaws.com:443";
    config.ice_servers[6].urls = "stun:stun.douyucdn.cn:18000";

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

std::string PeerManager::createOffer() {
    if (!pc_) return "";
    const char* sdp = peer_connection_create_offer(pc_);
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
        int ret = peer_connection_create_datachannel_sid(pc_,
            DATA_CHANNEL_RELIABLE, 0, 0,
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
                              bool replace_existing) {
    if (!data || len == 0 || len > kMaxOutboundPayloadBytes ||
        !connected_.load()) {
        if (data && len > kMaxOutboundPayloadBytes) {
            logOutboundDrop("payload_too_large", type,
                            static_cast<int>(std::min<size_t>(len, INT32_MAX)));
        }
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        if (replace_existing) {
            for (auto& command : outbound_commands_) {
                if (command.type == type) {
                    command.payload.assign(data, data + len);
                    command.id = next_outbound_command_id_++;
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
        outbound_commands_.push_back(std::move(command));
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
        return true;
    } catch (...) {
        return false;
    }
}

bool PeerManager::sendInputData(const uint8_t* data, size_t len) {
    return enqueueData(OutboundType::InputReliable, data, len, false);
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
    return isReliableCommand(type) || type == OutboundType::InputLatest;
}

int PeerManager::outboundPriority(OutboundType type) {
    switch (type) {
        case OutboundType::Pli: return 0;
        case OutboundType::Nack: return 1;
        case OutboundType::ReceiverFeedback: return 2;
        case OutboundType::InputReliable:
        case OutboundType::Control:
        case OutboundType::Message: return 3;
        case OutboundType::InputLatest: return 4;
    }
    return 5;
}

bool PeerManager::selectOutboundCommand(OutboundCommand& command,
                                        bool allow_sctp) const {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    auto selected = outbound_commands_.end();
    int selected_priority = 6;
    for (auto it = outbound_commands_.begin();
         it != outbound_commands_.end(); ++it) {
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

bool PeerManager::consumeReliableSendFailure() {
    return reliable_send_failed_.exchange(false);
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
         command.type == OutboundType::InputLatest ||
         command.type == OutboundType::Control ||
         command.type == OutboundType::Message) &&
        !data_channels_created_ && !createDataChannels()) {
        return -1;
    }
    switch (command.type) {
        case OutboundType::InputReliable:
        case OutboundType::InputLatest:
            return peer_connection_datachannel_send_sid_binary(
                pc_,
                reinterpret_cast<char*>(const_cast<uint8_t*>(command.payload.data())),
                command.payload.size(),
                xstreamingDataChannelSid("input"));
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

void PeerManager::drainOutboundCommands(
    std::chrono::steady_clock::time_point deadline) {
    constexpr size_t kMaxCommandsPerPump = 8;
    bool allow_sctp = true;
    for (size_t sent = 0; sent < kMaxCommandsPerPump; ++sent) {
        OutboundCommand command;
        try {
            if (!selectOutboundCommand(command, allow_sctp)) return;
        } catch (...) {
            logOutboundDrop("snapshot_failed", OutboundType::InputReliable);
            return;
        }
        if (command.expires_at.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() >= command.expires_at) {
            completeOutboundCommand(command, 0);
            logOutboundDrop("expired", command.type);
            continue;
        }
        const int result = sendOutboundCommand(command);
        if (completeOutboundCommand(command, result)) {
            if (!isSctpCommand(command.type)) return;
            allow_sctp = false;
            continue;
        }
        if (sent > 0 && std::chrono::steady_clock::now() >= deadline) return;
    }
}

bool PeerManager::completeOutboundCommand(const OutboundCommand& command,
                                          int result) {
    const bool transient =
        result < 0 && peer_connection_is_transient_send_error(result);
    const bool keep_for_retry =
        (transient && isReliableCommand(command.type)) ||
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
                   (command.type == OutboundType::Nack ||
                    command.type == OutboundType::Pli)) {
            queued->attempts++;
        }
    }
    if (result >= 0) return false;
    if (keep_for_retry) {
        logOutboundDrop("send_retry", command.type, result);
        return true;
    }
    logOutboundDrop(transient ? "transient_drop" : "send_failed",
                    command.type, result);
    if (!transient && isReliableCommand(command.type)) {
        reliable_send_failed_ = true;
    }
    return false;
}

void PeerManager::clearOutboundCommands() {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    outbound_commands_.clear();
    next_outbound_command_id_ = 1;
}

bool PeerManager::isConnected() const {
    return connected_;
}

bool PeerManager::isDataChannelReady() const {
    return pc_ && peer_connection_is_datachannel_connected(pc_);
}

void PeerManager::setMediaEnabled(bool enabled) {
    if (pc_) {
        lunar::diagnosticLog("webrtc", "set media enabled=%s", enabled ? "true" : "false");
        peer_connection_set_media_enabled(pc_, enabled ? 1 : 0);
    }
}

PeerMediaStats PeerManager::getMediaStats() const {
    PeerMediaStats stats = {};
    if (pc_) {
        peer_connection_get_media_stats(
            pc_, static_cast<PeerConnectionMediaStats*>(&stats));
    }
    const auto video = video_jitter_.stats();
    stats.video_rtp_packets = video.packets;
    stats.video_rtp_sequence_gaps = video.sequence_gaps;
    stats.video_rtp_missing_packets = video.missing_packets;
    stats.video_h264_frames = video.frames;
    stats.video_h264_corrupt_frames = video.corrupt_frames;
    stats.video_h264_unsupported_nalus = video.unsupported_nalus;
    stats.video_h264_overflow_frames = video.overflow_frames;
    stats.video_h264_max_frame_bytes = video.max_frame_bytes;
    stats.video_rtp_highest_seq_ext = video.highest_sequence;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    stats.video_rtp_nacks = video.nacks;
    stats.video_rtp_resyncs = video.resyncs;
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
    stats.video_waiting_keyframe = video_jitter_.waitingForKeyframe();
#endif
    return stats;
}

void PeerManager::processEvents() {
    if (pc_) {
        int log_index = process_event_logs_.fetch_add(1);
        if (log_index < 16) {
            lunar::diagnosticLog("webrtc", "processEvents begin index=%d", log_index);
        }
        constexpr int kMaxDrainPasses = 8;
        constexpr auto kMaxDrainTime = std::chrono::milliseconds(3);
        const auto drain_start = std::chrono::steady_clock::now();
        int drain_passes = 0;
        int drain_work = 0;
        while (drain_passes < kMaxDrainPasses) {
            const int work = peer_connection_loop(pc_);
            drain_work += work;
            drain_passes++;
            if (work <= 0 ||
                std::chrono::steady_clock::now() - drain_start >= kMaxDrainTime) {
                break;
            }
        }
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
        drainOutboundCommands(std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(1));
        PeerConnectionMediaStats network_stats = {};
        peer_connection_get_media_stats(pc_, &network_stats);
        if (network_stats.ice_rtt_ms > 0) {
            const uint64_t rtt_ms = network_stats.ice_rtt_ms;
            const uint64_t hold_ms = std::max<uint64_t>(
                VideoRtpJitterBuffer::kDefaultHoldMs,
                std::min<uint64_t>(180,
                                   rtt_ms * 2 + 20));
            video_jitter_.setHoldMs(hold_ms);
            const uint64_t recovery_hold_ms = std::max<uint64_t>(
                VideoRtpJitterBuffer::kDefaultRecoveryHoldMs,
                std::min<uint64_t>(VideoRtpJitterBuffer::kMaxRecoveryHoldMs,
                                   rtt_ms + 150));
            video_jitter_.setRecoveryHoldMs(recovery_hold_ms);
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
