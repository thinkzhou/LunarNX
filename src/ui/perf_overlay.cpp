#ifdef __SWITCH__
#include "perf_overlay.h"
#include "ui_style.h"

namespace lunar::ui {

PerfOverlay::PerfOverlay(const stream::PerfStats* perf) : Box(brls::Axis::COLUMN), perf_(perf) {
    const auto& p = uiPalette();
    this->setBackgroundColor(p.stream_overlay);
    this->setBorderThickness(1);
    this->setBorderColor(p.border);
    this->setCornerRadius(14);
    this->setPadding(14, 16, 14, 16);
    this->setWidth(500);
    this->setVisibility(brls::Visibility::GONE); // Hidden by default, R3 to show

    auto* title = new brls::Label();
    title->setHeight(30);
    title->setText(brls::getStr("lunarnx/perf/title"));
    title->setFontSize(12);
    title->setTextColor(p.accent);
    title->setVerticalAlign(brls::VerticalAlign::CENTER);
    this->addView(title);

    label_ = new brls::Label();
    label_->setFontSize(13);
    label_->setTextColor(nvgRGB(244, 248, 243));
    label_->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    this->addView(label_);
}

void PerfOverlay::update(float fps, const std::string& resolution,
                         const std::string& video_backend) {
    if (!visible_ || !label_) return;

    uint32_t video = perf_->video_frames.load();
    uint32_t audio = perf_->audio_frames.load();
    uint32_t audio_drops = perf_->audio_drops.load();
    uint32_t audio_queued = perf_->audio_queued_buffers.load();
    uint32_t audio_high = perf_->audio_queue_high_watermark.load();
    uint32_t audio_latency = perf_->audio_latency_ms.load();
    uint32_t audio_latency_high = perf_->audio_latency_high_watermark_ms.load();
    uint32_t audio_buffer = perf_->audio_buffer_ms.load();
    uint32_t audio_overflow = perf_->audio_overflow_ms.load();
    uint32_t input = perf_->input_packets.load();
    uint32_t video_aus = perf_->video_packets.load();
    uint32_t audio_aus = perf_->audio_packets.load();
    uint32_t rtp_video = perf_->rtp_video_packets.load();
    uint32_t rtp_audio = perf_->rtp_audio_packets.load();
    uint32_t video_missing = perf_->rtp_video_missing_packets.load();
    uint32_t h264_corrupt = perf_->h264_corrupt_frames.load();
    uint32_t srtp_fail = perf_->srtp_rtp_decrypt_failures.load();
    uint32_t decode_errors = perf_->video_decode_errors.load();
    float dec_ms   = perf_->avg_decode_ms();
    float render_ms = perf_->avg_render_submit_ms();
    float post_ms = perf_->avg_post_process_ms();
    float upscaling_ms = perf_->avg_upscaling_ms();
    float sharpening_ms = perf_->avg_sharpening_ms();
    float dithering_ms = perf_->avg_dithering_ms();

    // Drop rate as percentage
    uint32_t recv = perf_->rtp_video_packets.load();
    uint32_t lost = perf_->rtp_video_missing_packets.load();
    float drop_pct = (recv + lost > 0) ? (100.0f * lost / (recv + lost)) : 0.0f;

    std::string text = brls::getStr(
        "lunarnx/perf/detail_frames", fps, video, audio, input);
    text += "\n" + brls::getStr("lunarnx/perf/detail_decode", dec_ms);
    text += "\n" + brls::getStr(
        "lunarnx/perf/detail_render", render_ms, post_ms);
    text += "\n" + brls::getStr(
        "lunarnx/perf/detail_filters", upscaling_ms, sharpening_ms,
        dithering_ms);
    text += "\n" + brls::getStr(
        "lunarnx/perf/detail_audio", audio_queued, audio_high,
        audio_latency, audio_latency_high, audio_buffer, audio_overflow,
        audio_drops);
    text += "\n" + brls::getStr(
        "lunarnx/perf/detail_access_units", video_aus, audio_aus,
        decode_errors);
    text += "\n" + brls::getStr(
        "lunarnx/perf/detail_rtp", rtp_video, rtp_audio, video_missing,
        h264_corrupt, srtp_fail);
    if (!resolution.empty()) {
        text += "\n" + brls::getStr(
            "lunarnx/perf/detail_resolution", resolution);
    }
    if (!video_backend.empty()) {
        text += "\n" + brls::getStr(
            "lunarnx/perf/detail_backend", video_backend);
    }
    text += "\n" + brls::getStr(
        "lunarnx/perf/detail_packet_loss", lost, drop_pct);

    label_->setText(text);
}

void PerfOverlay::toggle() {
    setVisible(!visible_);
}

void PerfOverlay::setVisible(bool visible) {
    visible_ = visible;
    this->setVisibility(visible_ ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
}

} // namespace lunar::ui
#endif
