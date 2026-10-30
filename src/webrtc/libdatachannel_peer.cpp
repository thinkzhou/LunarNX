#include "libdatachannel_peer.h"
#include "xstreaming_data_channels.h"
#include "../diagnostics.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>
#ifndef LUNARNX_ENABLE_LIBDATACHANNEL
#define LUNARNX_ENABLE_LIBDATACHANNEL 0
#endif
#if LUNARNX_ENABLE_LIBDATACHANNEL
#include <rtc/rtc.hpp>
#include "../../tools/libdatachannel_switch/compat/background.hpp"

namespace lunar::webrtc {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxMediaBytes = 4 * 1024 * 1024;
constexpr size_t kMaxControlEvents = 256;
constexpr uint32_t kReportSsrc = 0x4c554e41;
uint16_t read16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
uint32_t read32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
struct Event {
    enum Kind { Candidate, Connection, Message, Video, Audio, Error } kind;
    std::vector<uint8_t> data;
    std::string text;
    uint16_t sid = 0;
    PeerConnectionState state = PEER_CONNECTION_NEW;
    Clock::time_point arrived = Clock::now();
};
struct Inbox {
    std::mutex mutex;
    std::deque<Event> control, media;
    size_t media_bytes = 0;
    bool closing = false;
    std::atomic<bool> failed{false};
    PeerConnectionMediaStats stats{};
    void push(Event event) noexcept {
        try {
            std::lock_guard<std::mutex> guard(mutex);
            if (closing) return;
            const bool isMedia = event.kind == Event::Video || event.kind == Event::Audio;
            if (isMedia) {
                ++stats.pump_socket_packets_total;
                if (event.kind == Event::Video) ++stats.video_rtp_packets;
                else ++stats.audio_rtp_packets;
                if (media.size() >= 4096 || media_bytes + event.data.size() > kMaxMediaBytes) {
                    ++stats.rtp_queue_drops; return;
                }
                media_bytes += event.data.size(); media.push_back(std::move(event));
                stats.rtp_queue_high_watermark = std::max(stats.rtp_queue_high_watermark, uint32_t(media.size()));
            } else if (control.size() < kMaxControlEvents) control.push_back(std::move(event));
            else failed = true;
        } catch (...) { failed = true; }
    }
};
}
struct LibdatachannelPeer::Impl {
    std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
    std::unique_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video, audio;
    std::array<std::shared_ptr<rtc::DataChannel>, 4> channels{};
    PeerConfiguration config{};
    void (*candidate)(char*, void*) = nullptr;
    void (*state)(PeerConnectionState, void*) = nullptr;
    void (*message)(char*, size_t, void*, uint16_t) = nullptr;
    bool media_enabled = true;
    bool failure_reported = false;
    uint32_t video_ssrc = 0;
    int rtcp(const uint8_t* data, size_t size) {
        try {
            if (!video || !video->isOpen()) return -1;
            // Track::send returns false when accepted into its send buffer.
            video->send(reinterpret_cast<const std::byte*>(data), size);
            return 0;
        } catch (...) { return -1; }
    }
};
LibdatachannelPeer::LibdatachannelPeer() : impl_(std::make_unique<Impl>()) {}
LibdatachannelPeer::~LibdatachannelPeer() { close(); }
bool LibdatachannelPeer::initialize(const PeerConfiguration& config,
    void (*candidate)(char*, void*), void (*state)(PeerConnectionState, void*),
    void (*message)(char*, size_t, void*, uint16_t)) {
    auto& p = *impl_; p.config = config; p.candidate = candidate; p.state = state; p.message = message;
    try {
        auto level = rtc::LogLevel::Warning;
#ifndef __SWITCH__
        if (std::getenv("LUNARNX_RTC_DEBUG")) level = rtc::LogLevel::Debug;
#endif
        rtc::InitLogger(level, [](rtc::LogLevel, const std::string& message) {
#ifndef __SWITCH__
            if (std::getenv("LUNARNX_RTC_DEBUG")) std::fprintf(stderr, "[rtc] %s\n", message.c_str());
#endif
            // Avoid dumping SDP, ICE credentials or full library payloads.
            lunar::diagnosticLog("libdatachannel", "library warning/error bytes=%zu", message.size());
        });
        rtc::Configuration options;
        options.disableAutoNegotiation = true;
        options.maxMessageSize = 65536;
        for (const auto& server : config.ice_servers) {
            if (server.urls) options.iceServers.emplace_back(server.urls);
        }
        p.pc = std::make_unique<rtc::PeerConnection>(options);
        auto inbox = p.inbox;
        p.pc->onLocalCandidate([inbox](rtc::Candidate candidate) {
            Event e{Event::Candidate}; e.text = "a=" + std::string(candidate) + "\r\n"; inbox->push(std::move(e));
        });
        p.pc->onStateChange([inbox](rtc::PeerConnection::State state) {
            Event e{Event::Connection};
            switch (state) {
                case rtc::PeerConnection::State::Connected: e.state = PEER_CONNECTION_COMPLETED; break;
                case rtc::PeerConnection::State::Disconnected: e.state = PEER_CONNECTION_DISCONNECTED; break;
                case rtc::PeerConnection::State::Failed: e.state = PEER_CONNECTION_FAILED; inbox->failed = true; break;
                case rtc::PeerConnection::State::Closed: e.state = PEER_CONNECTION_CLOSED; break;
                default: e.state = PEER_CONNECTION_CHECKING; break;
            }
            inbox->push(std::move(e));
        });
        rtc::Description::Video video("0", rtc::Description::Direction::RecvOnly);
        video.addH264Codec(102, "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e01f;max-fs=3600;max-mbps=108000");
        p.video = p.pc->addTrack(video);
        rtc::Description::Audio audio("1", rtc::Description::Direction::RecvOnly);
        audio.addOpusCodec(111); p.audio = p.pc->addTrack(audio);
        auto receive = [inbox](Event::Kind kind, rtc::binary packet) {
            if (packet.size() < 12 || packet.size() > 2048) return;
            const auto* bytes = reinterpret_cast<const uint8_t*>(packet.data());
            if ((bytes[0] >> 6) != 2 || (bytes[1] >= 192 && bytes[1] <= 223)) return;
            Event e{kind}; e.data.assign(bytes, bytes + packet.size()); inbox->push(std::move(e));
        };
        p.video->onMessage([receive](rtc::binary packet) { receive(Event::Video, std::move(packet)); }, nullptr);
        p.audio->onMessage([receive](rtc::binary packet) { receive(Event::Audio, std::move(packet)); }, nullptr);
        size_t index = 0;
        for (const auto& channel : kXStreamingDataChannels) {
            rtc::DataChannelInit init;
            init.id = channel.sid; init.protocol = channel.protocol;
            init.reliability.unordered = !channel.ordered;
            if (channel.max_retransmits >= 0) init.reliability.maxRetransmits = channel.max_retransmits;
            auto dc = p.pc->createDataChannel(channel.label, init);
            const uint16_t sid = channel.sid;
            dc->onMessage([inbox, sid](rtc::message_variant message) {
                Event e{Event::Message}; e.sid = sid;
                if (auto* binary = std::get_if<rtc::binary>(&message)) {
                    const auto* bytes = reinterpret_cast<const uint8_t*>(binary->data());
                    e.data.assign(bytes, bytes + binary->size());
                } else {
                    const auto& text = std::get<std::string>(message); e.data.assign(text.begin(), text.end());
                }
                inbox->push(std::move(e));
            });
            const std::string label = channel.label;
            dc->onClosed([inbox, label] {
                lunar::persistentEventLog("libdatachannel", "channel closed label=%s", label.c_str());
                inbox->failed = true;
            });
            dc->onError([inbox, label](std::string error) {
                lunar::persistentEventLog("libdatachannel", "channel error label=%s bytes=%zu", label.c_str(), error.size());
#ifndef __SWITCH__
                if (std::getenv("LUNARNX_RTC_DEBUG")) std::fprintf(stderr, "[rtc] channel %s: %s\n", label.c_str(), error.c_str());
#endif
                inbox->failed = true;
            });
            p.channels[index++] = std::move(dc);
        }
        return true;
    } catch (const std::exception& error) {
        lunar::diagnosticLog("libdatachannel", "initialize failed: %s", error.what()); close(); return false;
    }
}
std::string LibdatachannelPeer::createOffer() {
    try { impl_->pc->setLocalDescription(rtc::Description::Type::Offer); return std::string(*impl_->pc->localDescription()); }
    catch (const std::exception& error) { lunar::diagnosticLog("libdatachannel", "offer failed: %s", error.what()); impl_->inbox->failed = true; return {}; }
}
void LibdatachannelPeer::setRemoteAnswer(const std::string& sdp) {
    try { impl_->pc->setRemoteDescription(rtc::Description(sdp, "answer")); }
    catch (const std::exception& error) { lunar::diagnosticLog("libdatachannel", "answer rejected: %s", error.what()); impl_->inbox->failed = true; }
}
void LibdatachannelPeer::addIceCandidate(const std::string& candidate) {
    try { if (candidate.find("candidate:") != std::string::npos) impl_->pc->addRemoteCandidate(rtc::Candidate(candidate, "0")); }
    catch (...) { impl_->inbox->failed = true; }
}
bool LibdatachannelPeer::channelsReady() const {
    if (impl_->inbox->failed) return false;
    for (const auto& channel : impl_->channels) if (!channel || !channel->isOpen()) return false;
    return true;
}
int LibdatachannelPeer::send(uint16_t sid, const uint8_t* data, size_t size, bool binary) {
    try {
        for (const auto& dc : impl_->channels) {
            if (!dc || dc->id() != sid) continue;
            if (!dc->isOpen()) return -1;
            // Do not let the library queue grow behind the replaceable input
            // queue. A successful send transfers ownership even when buffered.
            if (dc->bufferedAmount() > 64 * 1024) return -1;
            if (binary) dc->send(reinterpret_cast<const std::byte*>(data), size);
            else dc->send(std::string(reinterpret_cast<const char*>(data), size));
            return 0;
        }
    } catch (...) { impl_->inbox->failed = true; }
    return -1;
}
int LibdatachannelPeer::sendNack(uint16_t pid, uint16_t blp) {
    if (!impl_->video_ssrc) return -1;
    uint8_t packet[16] = {0x81, 205, 0, 3};
    put32(packet + 4, kReportSsrc); put32(packet + 8, impl_->video_ssrc);
    put16(packet + 12, pid); put16(packet + 14, blp); return impl_->rtcp(packet, sizeof(packet));
}
int LibdatachannelPeer::requestKeyframe() {
    if (!impl_->video_ssrc) return -1;
    uint8_t packet[12] = {0x81, 206, 0, 2};
    put32(packet + 4, kReportSsrc); put32(packet + 8, impl_->video_ssrc); return impl_->rtcp(packet, sizeof(packet));
}
int LibdatachannelPeer::sendFeedback(uint8_t fraction, uint32_t lost, uint32_t highest, uint32_t bitrate) {
    if (!impl_->video_ssrc || !bitrate) return -1;
    uint8_t packet[76] = {0x81, 201, 0, 7};
    put32(packet + 4, kReportSsrc); put32(packet + 8, impl_->video_ssrc);
    put32(packet + 12, uint32_t(fraction) << 24 | (lost & 0xffffff)); put32(packet + 16, highest);
    // Compound RR + SDES/CNAME + REMB, as in the legacy path. LSR/DLSR are
    // omitted until this backend has a valid sender-report clock sample.
    packet[32] = 0x81; packet[33] = 202; put16(packet + 34, 4); put32(packet + 36, kReportSsrc);
    packet[40] = 1; packet[41] = 7; std::memcpy(packet + 42, "lunarnx", 7);
    packet[52] = 0x8f; packet[53] = 206; put16(packet + 54, 5); put32(packet + 56, kReportSsrc);
    std::memcpy(packet + 64, "REMB", 4); packet[68] = 1;
    uint32_t exponent = 0;
    while (bitrate > 0x3ffff) { bitrate >>= 1; ++exponent; }
    packet[69] = uint8_t(exponent << 2 | (bitrate >> 16)); packet[70] = bitrate >> 8; packet[71] = bitrate;
    put32(packet + 72, impl_->video_ssrc); return impl_->rtcp(packet, sizeof(packet));
}
void LibdatachannelPeer::setMediaEnabled(bool enabled) { impl_->media_enabled = enabled; }
int LibdatachannelPeer::loop() {
    auto& p = *impl_; int work = 0;
    if (p.inbox->failed && !p.failure_reported) {
        p.failure_reported = true;
        if (p.state) p.state(PEER_CONNECTION_FAILED, p.config.user_data);
    }
    for (int i = 0; i < 128; ++i) {
        Event event{Event::Error};
        {
            std::lock_guard<std::mutex> guard(p.inbox->mutex);
            if (!p.inbox->control.empty()) { event = std::move(p.inbox->control.front()); p.inbox->control.pop_front(); }
            else if (p.media_enabled && !p.inbox->media.empty()) {
                event = std::move(p.inbox->media.front()); p.inbox->media.pop_front();
                p.inbox->media_bytes -= event.data.size();
                ++p.inbox->stats.pump_rtp_packets_decoded_total;
            } else break;
        }
        ++work;
        switch (event.kind) {
            case Event::Candidate: if (p.candidate) p.candidate(event.text.data(), p.config.user_data); break;
            case Event::Connection: if (p.state) p.state(p.inbox->failed ? PEER_CONNECTION_FAILED : event.state, p.config.user_data); break;
            case Event::Message: if (p.message) p.message(reinterpret_cast<char*>(event.data.data()), event.data.size(), p.config.user_data, event.sid); break;
            case Event::Video:
                p.video_ssrc = read32(event.data.data() + 8);
                if (p.config.onvideotrack) p.config.onvideotrack(event.data.data(), event.data.size(), read16(event.data.data() + 2), read32(event.data.data() + 4), p.config.user_data);
                break;
            case Event::Audio: {
                const auto& data = event.data;
                size_t offset = 12 + (data[0] & 15) * 4;
                if (offset > data.size()) break;
                if (data[0] & 0x10) {
                    if (offset + 4 > data.size()) break;
                    offset += 4 + size_t(read16(data.data() + offset + 2)) * 4;
                }
                size_t end = data.size();
                if (data[0] & 0x20) {
                    if (data.back() == 0 || data.back() > end) break;
                    end -= data.back();
                }
                if (offset < end && p.config.onaudiotrack) p.config.onaudiotrack(event.data.data() + offset, end - offset, read16(data.data() + 2), read32(data.data() + 4), p.config.user_data);
                break;
            }
            case Event::Error: break;
        }
    }
    return work;
}
PeerConnectionMediaStats LibdatachannelPeer::stats() const {
    auto& p = *impl_;
    PeerConnectionMediaStats stats{};
    {
        std::lock_guard<std::mutex> guard(p.inbox->mutex);
        stats = p.inbox->stats; stats.rtp_queue_depth = p.inbox->media.size();
        if (!p.inbox->media.empty()) stats.rtp_queue_oldest_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - p.inbox->media.front().arrived).count();
    }
    if (p.pc) {
        // PeerConnection::rtt() is SCTP RTT, not STUN/ICE RTT. Leave this
        // metric unavailable instead of feeding a different signal to policy.
        rtc::Candidate local, remote;
        if (p.pc->getSelectedCandidatePair(&local, &remote)) {
            local.resolve(rtc::Candidate::ResolveMode::Simple);
            remote.resolve(rtc::Candidate::ResolveMode::Simple);
            stats.ice_pair_selected = 1;
            auto copy = [](const rtc::Candidate& candidate, char* address, size_t capacity, uint16_t& port, uint8_t& kind) {
                const auto text = candidate.address().value_or("");
                std::snprintf(address, capacity, "%s", text.c_str()); port = candidate.port().value_or(0);
                switch (candidate.type()) {
                    case rtc::Candidate::Type::Host: kind = 0; break;
                    case rtc::Candidate::Type::ServerReflexive: kind = 1; break;
                    case rtc::Candidate::Type::PeerReflexive: kind = 2; break;
                    case rtc::Candidate::Type::Relayed: kind = 3; break;
                    default: kind = 0; break;
                }
            };
            copy(local, stats.ice_local_address, sizeof(stats.ice_local_address), stats.ice_local_port, stats.ice_local_candidate_type);
            copy(remote, stats.ice_remote_address, sizeof(stats.ice_remote_address), stats.ice_remote_port, stats.ice_remote_candidate_type);
        }
    }
    return stats;
}
void LibdatachannelPeer::close() {
    if (!impl_) return;
    auto& p = *impl_;
    { std::lock_guard<std::mutex> guard(p.inbox->mutex); p.inbox->closing = true; p.inbox->control.clear(); p.inbox->media.clear(); p.inbox->media_bytes = 0; }
    if (!p.pc) return;
    for (auto& dc : p.channels) { if (dc) dc->resetCallbacks(); dc.reset(); }
    if (p.video) p.video->resetCallbacks();
    if (p.audio) p.audio->resetCallbacks();
    p.video.reset(); p.audio.reset(); p.pc->resetCallbacks(); p.pc->close(); p.pc.reset();
    rtc::Cleanup().wait();
    joinRtcTasks();
}
}
#else
namespace lunar::webrtc {
struct LibdatachannelPeer::Impl {};
LibdatachannelPeer::LibdatachannelPeer() = default;
LibdatachannelPeer::~LibdatachannelPeer() = default;
bool LibdatachannelPeer::initialize(const PeerConfiguration&, void (*)(char*, void*), void (*)(PeerConnectionState, void*), void (*)(char*, size_t, void*, uint16_t)) { return false; }
std::string LibdatachannelPeer::createOffer() { return {}; }
void LibdatachannelPeer::setRemoteAnswer(const std::string&) {}
void LibdatachannelPeer::addIceCandidate(const std::string&) {}
bool LibdatachannelPeer::channelsReady() const { return false; }
int LibdatachannelPeer::send(uint16_t, const uint8_t*, size_t, bool) { return -1; }
int LibdatachannelPeer::sendNack(uint16_t, uint16_t) { return -1; }
int LibdatachannelPeer::requestKeyframe() { return -1; }
int LibdatachannelPeer::sendFeedback(uint8_t, uint32_t, uint32_t, uint32_t) { return -1; }
void LibdatachannelPeer::setMediaEnabled(bool) {}
int LibdatachannelPeer::loop() { return 0; }
PeerConnectionMediaStats LibdatachannelPeer::stats() const { return {}; }
void LibdatachannelPeer::close() {}
}
#endif
