#pragma once
#include "../common.h"
#include <chrono>
#include <atomic>
#include <cstdint>

namespace lunar::stream {

struct PerfStats {
    std::atomic<uint32_t> video_frames{0};
    std::atomic<uint32_t> audio_frames{0};
    std::atomic<uint32_t> audio_drops{0};
    std::atomic<uint32_t> audio_queued_buffers{0};
    std::atomic<uint32_t> audio_queue_high_watermark{0};
    std::atomic<uint32_t> audio_latency_ms{0};
    std::atomic<uint32_t> audio_latency_high_watermark_ms{0};
    std::atomic<uint32_t> audio_buffer_ms{0};
    std::atomic<uint32_t> audio_overflow_ms{0};
    std::atomic<uint32_t> input_packets{0};
    std::atomic<uint32_t> video_packets{0};
    std::atomic<uint32_t> audio_packets{0};
    // Encoded H.264 access-unit bytes, used for interval video bitrate.
    std::atomic<uint64_t> encoded_video_bytes{0};
    // Raw RTP video payload bytes, including packets later discarded by the
    // jitter buffer. This is the useful network bitrate signal during loss.
    std::atomic<uint64_t> received_video_bytes{0};
    // Local pipeline drops; the HUD also includes decode and corrupt-frame drops.
    std::atomic<uint32_t> video_frame_drops{0};
    // Smoothed frame transit variation, derived from arrival and mapped RTP time.
    std::atomic<uint64_t> video_jitter_us{0};
    // RTT measured by the selected ICE pair's Binding connectivity check.
    std::atomic<uint32_t> network_rtt_ms{0};
    std::atomic<uint32_t> video_decode_errors{0};

    std::atomic<uint64_t> decode_total_us{0};
    std::atomic<uint32_t> decode_samples{0};
    std::atomic<uint64_t> render_submit_total_us{0};
    std::atomic<uint32_t> render_submit_samples{0};
    std::atomic<uint32_t> post_processed_frames{0};
    std::atomic<uint64_t> post_process_total_us{0};
    std::atomic<uint32_t> post_process_samples{0};
    std::atomic<uint32_t> dithered_frames{0};
    std::atomic<uint64_t> dithering_total_us{0};
    std::atomic<uint32_t> dithering_samples{0};
    std::atomic<uint32_t> upscaled_frames{0};
    std::atomic<uint64_t> upscaling_total_us{0};
    std::atomic<uint32_t> upscaling_samples{0};
    std::atomic<uint32_t> sharpened_frames{0};
    std::atomic<uint64_t> sharpening_total_us{0};
    std::atomic<uint32_t> sharpening_samples{0};

    std::atomic<float> packet_loss_fraction{0.0f};
    std::atomic<uint32_t> packets_received{0};
    std::atomic<uint32_t> packets_lost{0};
    std::atomic<uint32_t> rtp_video_packets{0};
    std::atomic<uint32_t> rtp_audio_packets{0};
    std::atomic<uint32_t> rtp_video_sequence_gaps{0};
    std::atomic<uint32_t> rtp_audio_sequence_gaps{0};
    std::atomic<uint32_t> rtp_video_missing_packets{0};
    std::atomic<uint32_t> rtp_audio_missing_packets{0};
    std::atomic<uint32_t> h264_frames{0};
    std::atomic<uint32_t> h264_corrupt_frames{0};
    std::atomic<uint32_t> h264_unsupported_nalus{0};
    std::atomic<uint32_t> h264_overflow_frames{0};
    std::atomic<uint32_t> h264_max_frame_bytes{0};
    std::atomic<uint32_t> rtp_queue_drops{0};
    std::atomic<uint32_t> rtp_queue_high_watermark{0};
    std::atomic<uint32_t> srtp_rtp_decrypt_failures{0};
    std::atomic<uint32_t> srtp_rtcp_decrypt_failures{0};
    std::atomic<int64_t> last_video_transit_ns{0};
    std::atomic<bool> video_transit_initialized{false};

    std::chrono::steady_clock::time_point start_time;

    void reset() {
        video_frames = 0; audio_frames = 0; audio_drops = 0;
        audio_queued_buffers = 0; audio_queue_high_watermark = 0;
        audio_latency_ms = 0; audio_latency_high_watermark_ms = 0;
        audio_buffer_ms = 0; audio_overflow_ms = 0; input_packets = 0;
        video_packets = 0; audio_packets = 0;
        encoded_video_bytes = 0;
        received_video_bytes = 0;
        video_frame_drops = 0; video_jitter_us = 0; network_rtt_ms = 0;
        video_decode_errors = 0;
        decode_total_us = 0; decode_samples = 0;
        render_submit_total_us = 0; render_submit_samples = 0;
        post_processed_frames = 0; post_process_total_us = 0; post_process_samples = 0;
        dithered_frames = 0; dithering_total_us = 0; dithering_samples = 0;
        upscaled_frames = 0; upscaling_total_us = 0; upscaling_samples = 0;
        sharpened_frames = 0; sharpening_total_us = 0; sharpening_samples = 0;
        packet_loss_fraction = 0; packets_received = 0; packets_lost = 0;
        rtp_video_packets = 0; rtp_audio_packets = 0;
        rtp_video_sequence_gaps = 0; rtp_audio_sequence_gaps = 0;
        rtp_video_missing_packets = 0; rtp_audio_missing_packets = 0;
        h264_frames = 0; h264_corrupt_frames = 0; h264_unsupported_nalus = 0;
        h264_overflow_frames = 0; h264_max_frame_bytes = 0;
        rtp_queue_drops = 0; rtp_queue_high_watermark = 0;
        srtp_rtp_decrypt_failures = 0; srtp_rtcp_decrypt_failures = 0;
        last_video_transit_ns = 0; video_transit_initialized = false;
        start_time = std::chrono::steady_clock::now();
    }

    float fps() const {
        auto now = std::chrono::steady_clock::now();
        auto sec = std::chrono::duration<float>(now - start_time).count();
        if (sec <= 0.01f) return 0;
        return video_frames.load() / sec;
    }

    float avg_decode_ms() const {
        auto s = decode_samples.load();
        if (s == 0) return 0;
        return (decode_total_us.load() / (float)s) / 1000.0f;
    }

    float avg_render_submit_ms() const {
        auto s = render_submit_samples.load();
        if (s == 0) return 0;
        return (render_submit_total_us.load() / (float)s) / 1000.0f;
    }

    float avg_post_process_ms() const {
        auto s = post_process_samples.load();
        if (s == 0) return 0;
        return (post_process_total_us.load() / (float)s) / 1000.0f;
    }

    float avg_dithering_ms() const {
        auto s = dithering_samples.load();
        if (s == 0) return 0;
        return (dithering_total_us.load() / (float)s) / 1000.0f;
    }

    float avg_upscaling_ms() const {
        auto s = upscaling_samples.load();
        if (s == 0) return 0;
        return (upscaling_total_us.load() / (float)s) / 1000.0f;
    }

    float avg_sharpening_ms() const {
        auto s = sharpening_samples.load();
        if (s == 0) return 0;
        return (sharpening_total_us.load() / (float)s) / 1000.0f;
    }

    void recordFrame() { video_frames++; }
    void recordAudioFrame() { audio_frames++; }
    void recordAudioDrop() { audio_drops++; }
    void recordAudioQueuedBuffers(uint32_t queued) {
        audio_queued_buffers = queued;
        uint32_t current = audio_queue_high_watermark.load();
        while (queued > current &&
               !audio_queue_high_watermark.compare_exchange_weak(current, queued)) {}
    }
    void recordAudioLatency(uint32_t latency_ms, uint32_t buffer_ms, uint32_t overflow_ms) {
        audio_latency_ms = latency_ms;
        audio_buffer_ms = buffer_ms;
        audio_overflow_ms = overflow_ms;
        uint32_t current = audio_latency_high_watermark_ms.load();
        while (latency_ms > current &&
               !audio_latency_high_watermark_ms.compare_exchange_weak(current, latency_ms)) {}
    }
    void recordInputPacket() { input_packets++; }
    void recordVideoPacket(size_t bytes, uint64_t presentation_timestamp_ns) {
        video_packets++;
        encoded_video_bytes += bytes;

        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t transit_ns = now_ns -
            static_cast<int64_t>(presentation_timestamp_ns);
        const int64_t previous = last_video_transit_ns.exchange(transit_ns);
        if (!video_transit_initialized.exchange(true)) return;

        const int64_t delta_ns = transit_ns >= previous
            ? transit_ns - previous
            : previous - transit_ns;
        const uint64_t sample_us = static_cast<uint64_t>(delta_ns / 1000);
        const uint64_t current_us = video_jitter_us.load();
        const uint64_t updated_us = sample_us >= current_us
            ? current_us + (sample_us - current_us) / 16
            : current_us - (current_us - sample_us) / 16;
        video_jitter_us = updated_us;
    }
    void recordVideoNetworkBytes(size_t bytes) { received_video_bytes += bytes; }
    void recordAudioPacket() { audio_packets++; }
    void recordVideoFrameDrop() { video_frame_drops++; }
    void recordVideoDecodeError() { video_decode_errors++; }
    void recordDecodeLatency(uint64_t us) { decode_total_us += us; decode_samples++; }
    void recordRenderSubmit(uint64_t us) { render_submit_total_us += us; render_submit_samples++; }
    void recordPostProcess(uint64_t us) { post_process_total_us += us; post_process_samples++; post_processed_frames++; }
    void recordDithering(uint64_t us) { dithering_total_us += us; dithering_samples++; dithered_frames++; }
    void recordUpscaling(uint64_t us) { upscaling_total_us += us; upscaling_samples++; upscaled_frames++; }
    void recordSharpening(uint64_t us) { sharpening_total_us += us; sharpening_samples++; sharpened_frames++; }
    void recordPacketLoss(float f) { packet_loss_fraction = f; }
    void recordPackets(uint32_t r, uint32_t l) { packets_received += r; packets_lost += l; }
    void setRtpStats(uint32_t video_rtp,
                     uint32_t audio_rtp,
                     uint32_t video_gaps,
                     uint32_t audio_gaps,
                     uint32_t video_missing,
                     uint32_t audio_missing,
                     uint32_t h264_ok,
                     uint32_t h264_corrupt,
                     uint32_t h264_unsupported,
                     uint32_t h264_overflow,
                     uint32_t h264_max_bytes,
                     uint32_t queue_drops,
                     uint32_t queue_high_watermark,
                     uint32_t srtp_rtp_failures,
                     uint32_t srtp_rtcp_failures,
                     uint32_t ice_rtt_ms) {
        rtp_video_packets = video_rtp;
        rtp_audio_packets = audio_rtp;
        rtp_video_sequence_gaps = video_gaps;
        rtp_audio_sequence_gaps = audio_gaps;
        rtp_video_missing_packets = video_missing;
        rtp_audio_missing_packets = audio_missing;
        h264_frames = h264_ok;
        h264_corrupt_frames = h264_corrupt;
        h264_unsupported_nalus = h264_unsupported;
        h264_overflow_frames = h264_overflow;
        h264_max_frame_bytes = h264_max_bytes;
        rtp_queue_drops = queue_drops;
        rtp_queue_high_watermark = queue_high_watermark;
        srtp_rtp_decrypt_failures = srtp_rtp_failures;
        srtp_rtcp_decrypt_failures = srtp_rtcp_failures;
        network_rtt_ms = ice_rtt_ms;
    }
};

} // namespace lunar::stream
