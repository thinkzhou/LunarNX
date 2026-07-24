#include "peer_manager.h"
#include "xbox_input_feedback.h"
#include "xstreaming_data_channels.h"
#include "../diagnostics.h"
#include <cJSON.h>
#include <peer.h>
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
    const uint64_t mapped_timestamp = self->video_clock_.map(timestamp, arrival_ns);
    int log_index = self->video_callback_logs_.fetch_add(1);
    if (log_index < 8) {
        lunar::diagnosticLog("webrtc", "video callback begin len=%zu has_cb=%s",
                             size,
                             self->callbacks_.on_video_frame ? "true" : "false");
    }
    if (self->callbacks_.on_video_frame) {
        self->callbacks_.on_video_frame(data, size, sequence, mapped_timestamp);
    }
    if (log_index < 8) {
        lunar::diagnosticLog("webrtc", "video callback done");
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
                    self->callbacks_.on_rumble(command.left_motor,
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
    audio_callback_logs_ = 0;
    process_event_logs_ = 0;
    rumble_parse_logs_ = 0;
    media_clock_start_ = std::chrono::steady_clock::now();
    video_clock_.reset();
    audio_clock_.reset();

    PeerConfiguration config = {};
    config.video_codec = CODEC_H264;
    config.audio_codec = CODEC_OPUS;
    config.datachannel = DATA_CHANNEL_STRING;

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
    audio_callback_logs_ = 0;
    process_event_logs_ = 0;
    rumble_parse_logs_ = 0;
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

bool PeerManager::sendInputData(const uint8_t* data, size_t len) {
    if (!pc_ || !connected_) return false;
    if (!data_channels_created_ && !createDataChannels()) return false;
    const uint16_t sid = xstreamingDataChannelSid("input");
    return peer_connection_datachannel_send_sid_binary(pc_,
        reinterpret_cast<char*>(const_cast<uint8_t*>(data)),
        len, sid) >= 0;
}

bool PeerManager::sendControlData(const uint8_t* data, size_t len) {
    if (!pc_ || !connected_) return false;
    if (!data_channels_created_ && !createDataChannels()) return false;
    const uint16_t sid = xstreamingDataChannelSid("control");
    lunar::diagnosticLog("webrtc", "send control datachannel sid=%u len=%zu", sid, len);
    const bool sent = peer_connection_datachannel_send_sid(pc_,
        reinterpret_cast<char*>(const_cast<uint8_t*>(data)),
        len, sid) >= 0;
    lunar::diagnosticLog("webrtc", "send control datachannel result=%s",
                         sent ? "true" : "false");
    return sent;
}

bool PeerManager::sendMessageData(const uint8_t* data, size_t len) {
    if (!pc_ || !connected_) return false;
    if (!data_channels_created_ && !createDataChannels()) return false;
    const uint16_t sid = xstreamingDataChannelSid("message");
    lunar::diagnosticLog("webrtc", "send message datachannel sid=%u len=%zu", sid, len);
    const bool sent = peer_connection_datachannel_send_sid(pc_,
        reinterpret_cast<char*>(const_cast<uint8_t*>(data)),
        len, sid) >= 0;
    lunar::diagnosticLog("webrtc", "send message datachannel result=%s",
                         sent ? "true" : "false");
    return sent;
}

bool PeerManager::requestVideoKeyframe() {
    if (!pc_ || !connected_) return false;
    const int ret = peer_connection_send_rtcp_pil(pc_, 0);
    lunar::diagnosticLog("webrtc", "send RTCP PLI result=%d", ret);
    return ret >= 0;
}

bool PeerManager::sendReceiverFeedback(uint32_t bitrate_bps) {
    if (!pc_ || !connected_) return false;
    const int ret = peer_connection_send_receiver_feedback(pc_, bitrate_bps);
    lunar::diagnosticLog("webrtc", "send RTCP RR+REMB bitrate_bps=%u result=%d",
                         bitrate_bps, ret);
    return ret >= 0;
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

PeerConnectionMediaStats PeerManager::getMediaStats() const {
    PeerConnectionMediaStats stats = {};
    if (pc_) {
        peer_connection_get_media_stats(pc_, &stats);
    }
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
        if (log_index < 16) {
            lunar::diagnosticLog("webrtc", "processEvents done index=%d", log_index);
        }
    }
}

void PeerManager::disconnect() {
    lunar::diagnosticLog("webrtc", "disconnect begin pc=%p initialized=%s",
                         static_cast<void*>(pc_),
                         initialized_ ? "true" : "false");
    if (pc_) {
        lunar::diagnosticLog("webrtc", "disconnect close begin");
        peer_connection_close(pc_);
        lunar::diagnosticLog("webrtc", "disconnect close done");
        lunar::diagnosticLog("webrtc", "disconnect destroy begin");
        peer_connection_destroy(pc_);
        lunar::diagnosticLog("webrtc", "disconnect destroy done");
        pc_ = nullptr;
    }
    connected_ = false;
    data_channels_created_ = false;
    if (initialized_) {
        lunar::diagnosticLog("webrtc", "disconnect peer_deinit begin");
        peer_deinit();
        lunar::diagnosticLog("webrtc", "disconnect peer_deinit done");
        initialized_ = false;
    }
    lunar::diagnosticLog("webrtc", "disconnect done");
}

} // namespace lunar::webrtc
