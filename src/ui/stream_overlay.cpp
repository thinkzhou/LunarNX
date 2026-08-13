#ifdef __SWITCH__
#include "stream_overlay.h"
#include "ui_style.h"

namespace lunar::ui {

StreamOverlay::StreamOverlay(const stream::PerfStats* perf, app::StreamPlatform platform)
    : Box(brls::Axis::ROW), perf_(perf), platform_(platform) {
    const auto& p = uiPalette();
    this->setPadding(8, 24, 4, 24);
    this->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
    this->setHeight(40);
    this->setJustifyContent(brls::JustifyContent::CENTER);
    this->setAlignItems(brls::AlignItems::CENTER);

    metrics_label_ = new brls::Label();
    metrics_label_->setFontSize(12.0f);
    metrics_label_->setTextColor(p.text);
    metrics_label_->setSingleLine(true);
    metrics_label_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    metrics_label_->setVerticalAlign(brls::VerticalAlign::CENTER);
    metrics_label_->setGrow(1.0f);
    this->addView(metrics_label_);
}

void StreamOverlay::update(float fps, const std::string& resolution,
                           const std::string& video_codec) {
    if (!metrics_label_ || !perf_) return;

    const auto now = std::chrono::steady_clock::now();
    const bool is_ps = platform_ == app::StreamPlatform::PlayStation;
    const uint64_t encoded_bytes = perf_->received_video_bytes.load();
    const uint64_t decode_total_ns = perf_->decode_total_ns.load();
    const uint64_t decode_total_us = is_ps
        ? decode_total_ns / 1000ULL : perf_->decode_total_us.load();
    const uint32_t decode_samples = perf_->decode_samples.load();
    if (last_sample_time_.time_since_epoch().count() == 0) {
        last_sample_time_ = now;
        last_encoded_bytes_ = encoded_bytes;
        last_decode_total_us_ = decode_total_us;
        last_decode_samples_ = decode_samples;
    } else {
        const float sample_seconds =
            std::chrono::duration<float>(now - last_sample_time_).count();
        if (sample_seconds >= 0.9f) {
            const uint64_t byte_delta = encoded_bytes >= last_encoded_bytes_
                ? encoded_bytes - last_encoded_bytes_
                : 0;
            const float interval_bitrate_mbps =
                static_cast<float>(byte_delta * 8.0) /
                sample_seconds / 1000000.0f;
            if (is_ps) {
                bitrate_samples_[bitrate_sample_index_] = interval_bitrate_mbps;
                bitrate_sample_index_ =
                    (bitrate_sample_index_ + 1) % bitrate_samples_.size();
                bitrate_sample_count_ = std::min(
                    bitrate_sample_count_ + 1, bitrate_samples_.size());
                float bitrate_sum = 0.0f;
                for (size_t i = 0; i < bitrate_sample_count_; ++i) {
                    bitrate_sum += bitrate_samples_[i];
                }
                bitrate_mbps_ = bitrate_sum /
                    static_cast<float>(bitrate_sample_count_);
            } else {
                bitrate_mbps_ = interval_bitrate_mbps;
            }

            const uint64_t decode_delta = decode_total_us >= last_decode_total_us_
                ? decode_total_us - last_decode_total_us_
                : 0;
            const uint32_t sample_delta = decode_samples >= last_decode_samples_
                ? decode_samples - last_decode_samples_
                : 0;
            if (sample_delta > 0) {
                decode_ms_ = static_cast<float>(decode_delta) /
                    static_cast<float>(sample_delta) / 1000.0f;
            }
            last_sample_time_ = now;
            last_encoded_bytes_ = encoded_bytes;
            last_decode_total_us_ = decode_total_us;
            last_decode_samples_ = decode_samples;
        }
    }

    const uint32_t rendered = perf_->video_frames.load();
    const uint32_t frame_drops = perf_->video_frame_drops.load() +
        perf_->video_decode_errors.load() + perf_->h264_corrupt_frames.load();
    const float frame_drop_pct = rendered + frame_drops > 0
        ? 100.0f * static_cast<float>(frame_drops) /
            static_cast<float>(rendered + frame_drops)
        : 0.0f;

    uint32_t received = perf_->rtp_video_packets.load();
    uint32_t lost = perf_->rtp_video_missing_packets.load();
    // Chiaki delivers complete samples, not RTP packets. Use its reported
    // frame loss when fine-grained RTP counters are unavailable.
    if (is_ps && received == 0 && lost == 0 && perf_->video_packets.load() > 0) {
        received = perf_->video_packets.load();
        lost = perf_->packets_lost.load();
    }
    float packet_loss_pct = received + lost > 0
        ? 100.0f * static_cast<float>(lost) /
            static_cast<float>(received + lost)
        : 0.0f;

    const uint32_t ps_rtt = perf_->ps_rtt_ms.load();
    const float ps_loss = perf_->ps_packet_loss_fraction.load();
    const uint32_t ps_frames_lost = perf_->ps_frames_lost.load();
    const bool has_ps_transport = is_ps;
    const float jitter_ms = static_cast<float>(perf_->video_jitter_us.load()) /
        1000.0f;
    const uint32_t rtt_ms = has_ps_transport ? ps_rtt : perf_->network_rtt_ms.load();
    if (has_ps_transport) packet_loss_pct = ps_loss * 100.0f;
    const uint32_t displayed_frame_drops = has_ps_transport
        ? ps_frames_lost : frame_drops;

    if (has_ps_transport) {
        metrics_label_->setText(brls::getStr(
            "lunarnx/perf/hud_metrics_ps",
            resolution, rtt_ms, bitrate_mbps_, packet_loss_pct, fps,
            decode_ms_, ps_frames_lost, video_codec));
        return;
    }

    metrics_label_->setText(brls::getStr(
        "lunarnx/perf/hud_metrics",
        resolution,
        rtt_ms,
        jitter_ms,
        fps,
        displayed_frame_drops,
        frame_drop_pct,
        lost,
        packet_loss_pct,
        bitrate_mbps_,
        decode_ms_));
}

} // namespace lunar::ui
#endif
