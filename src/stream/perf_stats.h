#pragma once
#include "../common.h"
#include "../diagnostics.h"
#include <chrono>
#include <atomic>
#include <cstddef>
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
    std::atomic<uint32_t> video_sync_drops{0};
    std::atomic<uint32_t> video_queue_drops{0};
    // Smoothed frame transit variation, derived from arrival and mapped RTP time.
    std::atomic<uint64_t> video_jitter_us{0};
    // RTT measured by the selected ICE pair's Binding connectivity check.
    std::atomic<uint32_t> network_rtt_ms{0};
    std::atomic<uint32_t> video_decode_errors{0};

#if LUNARNX_DROP_DIAGNOSTIC_LOG
    // Sparse drop-diagnostic context. These are cheap rolling samples; file
    // output occurs only when an actual drop/error counter increases.
    std::atomic<uint32_t> video_queue_packets{0};
    std::atomic<uint64_t> video_queue_bytes{0};
    std::atomic<uint32_t> video_queue_oldest_age_ms{0};
    std::atomic<uint32_t> video_queue_high_watermark_packets{0};
    std::atomic<uint64_t> last_video_au_bytes{0};
    std::atomic<uint64_t> last_video_au_pts_ns{0};
    std::atomic<uint64_t> last_video_au_queue_age_us{0};
    std::atomic<bool> last_video_au_idr{false};
    std::atomic<int64_t> last_av_raw_delay_ns{0};
    std::atomic<int64_t> last_av_policy_delay_ns{0};
    std::atomic<int64_t> last_av_audio_age_ms{-1};
    std::atomic<uint64_t> last_av_master_pts_ns{0};
    std::atomic<bool> last_av_using_audio_master{false};
    std::atomic<uint64_t> last_decode_us{0};
    std::atomic<uint64_t> decode_window_max_us{0};
    std::atomic<uint64_t> last_render_submit_us{0};
    std::atomic<uint64_t> render_window_max_us{0};
    std::atomic<uint64_t> last_present_wait_us{0};
    std::atomic<uint64_t> present_wait_window_max_us{0};
    std::atomic<uint32_t> drop_diagnostic_events{0};
    std::atomic<uint64_t> last_video_loss_observed_ms{0};
    std::atomic<uint32_t> last_video_missing_delta{0};
    std::atomic<uint32_t> last_video_gap_delta{0};
    std::atomic<uint32_t> last_audio_missing_delta{0};
    std::atomic<uint32_t> keep_alive_interval_seconds{0};
    std::atomic<uint32_t> last_keep_alive_duration_ms{0};
    std::atomic<uint64_t> last_keep_alive_completed_ms{0};
    std::atomic<bool> last_keep_alive_ok{false};
    std::atomic<uint32_t> keep_alive_exception_count{0};
    std::atomic<uint64_t> last_keep_alive_exception_ms{0};
    std::atomic<uint32_t> token_refresh_exception_count{0};
    std::atomic<uint64_t> last_token_refresh_exception_ms{0};
    std::atomic<uint64_t> last_webrtc_pump_gap_us{0};
    std::atomic<uint64_t> max_webrtc_pump_gap_us{0};
    std::atomic<uint64_t> last_webrtc_pump_duration_us{0};
    std::atomic<uint64_t> max_webrtc_pump_duration_us{0};
    std::atomic<uint32_t> rtp_queue_drop_delta_diag{0};
    std::atomic<uint32_t> video_rtp_highest_sequence_diag{0};
    std::atomic<uint32_t> video_rtp_nacks_diag{0};
    std::atomic<uint32_t> video_rtp_missing_detected_diag{0};
    std::atomic<uint32_t> video_rtp_resyncs_diag{0};
    std::atomic<uint32_t> video_rtp_last_gap_packets_diag{0};
    std::atomic<uint32_t> video_rtp_ssrc_diag{0};
    std::atomic<uint32_t> video_rtp_ssrc_changes_diag{0};
    std::atomic<uint32_t> video_rtp_arrival_age_ms_diag{0};
    std::atomic<uint32_t> video_rtp_last_arrival_gap_ms_diag{0};
    std::atomic<uint32_t> video_rtp_max_arrival_gap_ms_diag{0};
    std::atomic<uint32_t> video_jitter_buffered_packets_diag{0};
    std::atomic<uint32_t> video_jitter_buffered_frames_diag{0};
    std::atomic<uint32_t> video_jitter_buffered_bytes_diag{0};
    std::atomic<bool> video_waiting_keyframe_diag{true};
    std::atomic<uint32_t> loss_episode_counter{0};
    std::atomic<uint32_t> active_loss_episode_id{0};
    std::atomic<uint64_t> active_loss_started_ms{0};
    std::atomic<bool> recovery_display_pending{false};
    std::atomic<bool> recovery_pli_logged{false};
#endif

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
        video_frame_drops = 0; video_sync_drops = 0; video_queue_drops = 0;
        video_jitter_us = 0; network_rtt_ms = 0;
        video_decode_errors = 0;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        video_queue_packets = 0; video_queue_bytes = 0;
        video_queue_oldest_age_ms = 0; video_queue_high_watermark_packets = 0;
        last_video_au_bytes = 0; last_video_au_pts_ns = 0;
        last_video_au_queue_age_us = 0; last_video_au_idr = false;
        last_av_raw_delay_ns = 0; last_av_policy_delay_ns = 0;
        last_av_audio_age_ms = -1; last_av_master_pts_ns = 0;
        last_av_using_audio_master = false;
        last_decode_us = 0; decode_window_max_us = 0;
        last_render_submit_us = 0; render_window_max_us = 0;
        last_present_wait_us = 0; present_wait_window_max_us = 0;
        drop_diagnostic_events = 0;
        last_video_loss_observed_ms = 0;
        last_video_missing_delta = 0; last_video_gap_delta = 0;
        last_audio_missing_delta = 0;
        keep_alive_interval_seconds = 0;
        last_keep_alive_duration_ms = 0;
        last_keep_alive_completed_ms = 0; last_keep_alive_ok = false;
        keep_alive_exception_count = 0; last_keep_alive_exception_ms = 0;
        token_refresh_exception_count = 0; last_token_refresh_exception_ms = 0;
        last_webrtc_pump_gap_us = 0; max_webrtc_pump_gap_us = 0;
        last_webrtc_pump_duration_us = 0; max_webrtc_pump_duration_us = 0;
        rtp_queue_drop_delta_diag = 0;
        video_rtp_highest_sequence_diag = 0;
        video_rtp_nacks_diag = 0; video_rtp_resyncs_diag = 0;
        video_rtp_missing_detected_diag = 0;
        video_rtp_last_gap_packets_diag = 0;
        video_rtp_ssrc_diag = 0; video_rtp_ssrc_changes_diag = 0;
        video_rtp_arrival_age_ms_diag = 0;
        video_rtp_last_arrival_gap_ms_diag = 0;
        video_rtp_max_arrival_gap_ms_diag = 0;
        video_jitter_buffered_packets_diag = 0;
        video_jitter_buffered_frames_diag = 0;
        video_jitter_buffered_bytes_diag = 0;
        video_waiting_keyframe_diag = true;
        loss_episode_counter = 0; active_loss_episode_id = 0;
        active_loss_started_ms = 0; recovery_display_pending = false;
        recovery_pli_logged = false;
#endif
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

    void recordFrame() {
        video_frames++;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        if (recovery_display_pending.exchange(false)) {
            logRecoveryDiagnostic("displayed", true);
            active_loss_episode_id = 0;
            active_loss_started_ms = 0;
            recovery_pli_logged = false;
        }
#endif
    }
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
#if LUNARNX_DROP_DIAGNOSTIC_LOG
    template <typename T>
    static void recordMaximum(std::atomic<T>& target, T sample) {
        T current = target.load();
        while (sample > current &&
               !target.compare_exchange_weak(current, sample)) {}
    }
#endif

    void recordVideoSyncDrop() {
        video_frame_drops++;
        video_sync_drops++;
    }
    void recordVideoQueueDrops(uint32_t count) {
        video_frame_drops += count;
        video_queue_drops += count;
    }
    void recordVideoDecodeError() { video_decode_errors++; }
    void recordVideoQueue(uint32_t packets, uint64_t bytes,
                          uint32_t oldest_age_ms) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        video_queue_packets = packets;
        video_queue_bytes = bytes;
        video_queue_oldest_age_ms = oldest_age_ms;
        recordMaximum(video_queue_high_watermark_packets, packets);
#else
        (void)packets; (void)bytes; (void)oldest_age_ms;
#endif
    }
    void recordVideoAccessUnit(size_t bytes, uint64_t pts_ns,
                               uint64_t queue_age_us, bool idr) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        last_video_au_bytes = bytes;
        last_video_au_pts_ns = pts_ns;
        last_video_au_queue_age_us = queue_age_us;
        last_video_au_idr = idr;
#else
        (void)bytes; (void)pts_ns; (void)queue_age_us; (void)idr;
#endif
    }
    void recordVideoTiming(int64_t raw_delay_ns, int64_t policy_delay_ns,
                           int64_t audio_age_ms, uint64_t master_pts_ns,
                           bool using_audio_master) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        last_av_raw_delay_ns = raw_delay_ns;
        last_av_policy_delay_ns = policy_delay_ns;
        last_av_audio_age_ms = audio_age_ms;
        last_av_master_pts_ns = master_pts_ns;
        last_av_using_audio_master = using_audio_master;
#else
        (void)raw_delay_ns; (void)policy_delay_ns; (void)audio_age_ms;
        (void)master_pts_ns; (void)using_audio_master;
#endif
    }
    void recordDecodeLatency(uint64_t us) {
        decode_total_us += us;
        decode_samples++;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        last_decode_us = us;
        recordMaximum(decode_window_max_us, us);
#endif
    }
    void recordRenderSubmit(uint64_t us) {
        render_submit_total_us += us;
        render_submit_samples++;
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        last_render_submit_us = us;
        recordMaximum(render_window_max_us, us);
#endif
    }
    void recordPresentWait(uint64_t us) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        last_present_wait_us = us;
        recordMaximum(present_wait_window_max_us, us);
#else
        (void)us;
#endif
    }
    void recordKeepAliveInterval(uint32_t seconds) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        keep_alive_interval_seconds = seconds;
#else
        (void)seconds;
#endif
    }
    void recordKeepAlive(uint32_t duration_ms, bool ok) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        last_keep_alive_duration_ms = duration_ms;
        last_keep_alive_ok = ok;
        last_keep_alive_completed_ms = lunar::diagnosticMonotonicMs();
#else
        (void)duration_ms; (void)ok;
#endif
    }
    void recordKeepAliveException() {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        keep_alive_exception_count++;
        last_keep_alive_exception_ms = lunar::diagnosticMonotonicMs();
#endif
    }
    void recordTokenRefreshException() {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        token_refresh_exception_count++;
        last_token_refresh_exception_ms = lunar::diagnosticMonotonicMs();
#endif
    }
    void recordWebRtcPump(uint64_t gap_us, uint64_t duration_us) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        last_webrtc_pump_gap_us = gap_us;
        last_webrtc_pump_duration_us = duration_us;
        recordMaximum(max_webrtc_pump_gap_us, gap_us);
        recordMaximum(max_webrtc_pump_duration_us, duration_us);
#else
        (void)gap_us; (void)duration_us;
#endif
    }
    void recordRecoveryPli(bool sent) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        if (active_loss_episode_id.load() != 0 &&
            !recovery_pli_logged.exchange(true)) {
            logRecoveryDiagnostic("pli_requested", sent);
        }
#else
        (void)sent;
#endif
    }
    uint32_t dropDiagnosticEventCount() const {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        return drop_diagnostic_events.load();
#else
        return 0;
#endif
    }
    uint64_t lastVideoAccessUnitQueueAgeUs() const {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        return last_video_au_queue_age_us.load();
#else
        return 0;
#endif
    }
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
                     uint32_t video_missing_detected,
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
                     uint32_t ice_rtt_ms,
                     uint32_t highest_sequence,
                     uint32_t nacks,
                     uint32_t resyncs,
                     uint32_t last_gap_packets,
                     uint32_t ssrc,
                     uint32_t ssrc_changes,
                     uint32_t arrival_age_ms,
                     uint32_t last_arrival_gap_ms,
                     uint32_t max_arrival_gap_ms,
                     uint32_t jitter_buffered_packets,
                     uint32_t jitter_buffered_frames,
                     uint32_t jitter_buffered_bytes,
                     bool waiting_keyframe) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        constexpr uint64_t kRtpLossEpisodeGapMs = 1000;
        const uint32_t previous_video_gaps = rtp_video_sequence_gaps.load();
        const uint32_t previous_video_missing_detected =
            video_rtp_missing_detected_diag.load();
        const uint32_t previous_audio_missing = rtp_audio_missing_packets.load();
        const uint32_t previous_h264_corrupt = h264_corrupt_frames.load();
        const uint32_t previous_queue_drops = rtp_queue_drops.load();
        const bool previous_waiting_keyframe = video_waiting_keyframe_diag.load();
#endif
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
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        rtp_queue_drop_delta_diag = queue_drops >= previous_queue_drops
            ? queue_drops - previous_queue_drops
            : 0;
        video_rtp_highest_sequence_diag = highest_sequence;
        video_rtp_nacks_diag = nacks;
        video_rtp_missing_detected_diag = video_missing_detected;
        video_rtp_resyncs_diag = resyncs;
        video_rtp_last_gap_packets_diag = last_gap_packets;
        video_rtp_ssrc_diag = ssrc;
        video_rtp_ssrc_changes_diag = ssrc_changes;
        video_rtp_arrival_age_ms_diag = arrival_age_ms;
        video_rtp_last_arrival_gap_ms_diag = last_arrival_gap_ms;
        video_rtp_max_arrival_gap_ms_diag = max_arrival_gap_ms;
        video_jitter_buffered_packets_diag = jitter_buffered_packets;
        video_jitter_buffered_frames_diag = jitter_buffered_frames;
        video_jitter_buffered_bytes_diag = jitter_buffered_bytes;
        video_waiting_keyframe_diag = waiting_keyframe;
        if (video_missing_detected > previous_video_missing_detected) {
            last_video_missing_delta =
                video_missing_detected - previous_video_missing_detected;
            last_video_gap_delta = video_gaps >= previous_video_gaps
                ? video_gaps - previous_video_gaps
                : 0;
            last_audio_missing_delta = audio_missing >= previous_audio_missing
                ? audio_missing - previous_audio_missing
                : 0;

            const uint64_t now_ms = lunar::diagnosticMonotonicMs();
            const uint64_t previous_loss_ms =
                last_video_loss_observed_ms.exchange(now_ms);
            if (previous_loss_ms == 0 ||
                now_ms - previous_loss_ms >= kRtpLossEpisodeGapMs) {
                active_loss_episode_id = loss_episode_counter.fetch_add(1) + 1;
                active_loss_started_ms = now_ms;
                recovery_display_pending = false;
                recovery_pli_logged = false;
                logVideoDropDiagnostic("rtp_loss",
                                       "video_missing_increased");
            }
        }
        if (h264_corrupt > previous_h264_corrupt) {
            logVideoDropDiagnostic("h264_corrupt",
                                   "rtp_reassembly_rejected_access_unit",
                                   static_cast<int>(h264_corrupt - previous_h264_corrupt));
        }
        if (active_loss_episode_id.load() != 0 &&
            previous_waiting_keyframe && !waiting_keyframe) {
            recovery_display_pending = true;
            logRecoveryDiagnostic("idr_received", true);
        }
#else
        (void)highest_sequence; (void)nacks; (void)resyncs;
        (void)last_gap_packets; (void)ssrc; (void)ssrc_changes;
        (void)arrival_age_ms; (void)last_arrival_gap_ms;
        (void)max_arrival_gap_ms; (void)jitter_buffered_packets;
        (void)jitter_buffered_frames; (void)jitter_buffered_bytes;
        (void)waiting_keyframe;
        (void)video_missing_detected;
#endif
    }

    void logRecoveryDiagnostic(const char* phase, bool ok) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        const uint32_t episode = active_loss_episode_id.load();
        if (episode == 0) return;
        const uint64_t now_ms = lunar::diagnosticMonotonicMs();
        const uint64_t started_ms = active_loss_started_ms.load();
        const uint64_t elapsed_ms = started_ms > 0 && now_ms >= started_ms
            ? now_ms - started_ms
            : 0;
        lunar::dropDiagnosticLog(
            "rtp-recovery",
            "episode=%u phase=%s elapsed_ms=%llu ok=%d missing=%u "
            "nacks=%u waiting_keyframe=%d rendered=%u",
            episode,
            phase ? phase : "unknown",
            static_cast<unsigned long long>(elapsed_ms),
            ok ? 1 : 0,
            rtp_video_missing_packets.load(),
            video_rtp_nacks_diag.load(),
            video_waiting_keyframe_diag.load() ? 1 : 0,
            video_frames.load());
#else
        (void)phase; (void)ok;
#endif
    }

    void logVideoDropDiagnostic(const char* source,
                                const char* reason,
                                int error_code = 0,
                                uint32_t error_flags = 0,
                                uint64_t frame_pts_ns = 0,
                                size_t access_unit_bytes = 0,
                                int frame_width = 0,
                                int frame_height = 0,
                                const char* nal_types = nullptr,
                                bool is_idr = false) {
#if LUNARNX_DROP_DIAGNOSTIC_LOG
        const uint32_t event = drop_diagnostic_events.fetch_add(1) + 1;
        const uint64_t au_bytes = access_unit_bytes > 0
            ? static_cast<uint64_t>(access_unit_bytes)
            : last_video_au_bytes.load();
        const uint64_t pts_ns = frame_pts_ns > 0
            ? frame_pts_ns
            : last_video_au_pts_ns.load();
        const bool idr = is_idr || last_video_au_idr.load();
        const uint64_t decode_max_us = decode_window_max_us.exchange(0);
        const uint64_t render_max_us = render_window_max_us.exchange(0);
        const uint64_t present_wait_max_us = present_wait_window_max_us.exchange(0);
        const uint32_t local_drops = video_frame_drops.load();
        const uint32_t decode_errors = video_decode_errors.load();
        const uint32_t corrupt = h264_corrupt_frames.load();
        const uint64_t now_ms = lunar::diagnosticMonotonicMs();
        const uint64_t keep_alive_completed_ms = last_keep_alive_completed_ms.load();
        const uint64_t keep_alive_age_ms = keep_alive_completed_ms > 0 &&
                now_ms >= keep_alive_completed_ms
            ? now_ms - keep_alive_completed_ms
            : 0;
        const uint64_t keep_alive_exception_ms =
            last_keep_alive_exception_ms.load();
        const uint64_t keep_alive_exception_age_ms =
            keep_alive_exception_count.load() > 0 &&
                now_ms >= keep_alive_exception_ms
            ? now_ms - keep_alive_exception_ms
            : 0;
        const uint64_t token_refresh_exception_ms =
            last_token_refresh_exception_ms.load();
        const uint64_t token_refresh_exception_age_ms =
            token_refresh_exception_count.load() > 0 &&
                now_ms >= token_refresh_exception_ms
            ? now_ms - token_refresh_exception_ms
            : 0;

        lunar::dropDiagnosticLog(
            "video-drop",
            "event=%u source=%s reason=%s err=%d flags=0x%x "
            "episode=%u "
            "frame=%dx%d pts_ns=%llu au_bytes=%llu nal=%s idr=%d "
            "av_raw_us=%lld av_policy_us=%lld audio_age_ms=%lld "
            "audio_master=%d master_pts_ns=%llu "
            "media_q_packets=%u media_q_bytes=%llu media_q_oldest_ms=%u "
            "media_q_high=%u au_queue_age_us=%llu "
            "decode_last_us=%llu decode_max_us=%llu "
            "render_last_us=%llu render_max_us=%llu "
            "present_wait_last_us=%llu present_wait_max_us=%llu "
            "audio_latency_ms=%u audio_buffer_ms=%u audio_queued=%u "
            "rendered=%u video_aus=%u audio_aus=%u "
            "rtp_video=%u missing=%u missing_delta=%u video_gap_delta=%u "
            "audio_missing=%u audio_missing_delta=%u jitter_us=%llu rtt_ms=%u "
            "pump_gap_us=%llu pump_gap_max_us=%llu "
            "pump_duration_us=%llu pump_duration_max_us=%llu "
            "arrival_age_ms=%u arrival_gap_ms=%u arrival_gap_max_ms=%u "
            "highest_seq=%u gap_packets=%u ssrc=0x%08x ssrc_changes=%u "
            "nacks=%u resyncs=%u jitter_q_packets=%u jitter_q_frames=%u "
            "jitter_q_bytes=%u waiting_keyframe=%d "
            "h264_corrupt=%u h264_overflow=%u h264_max_bytes=%u "
            "rtp_queue_high=%u rtp_queue_drop=%u "
            "rtp_queue_drop_delta=%u srtp_fail=%u "
            "keepalive_interval_s=%u keepalive_duration_ms=%u "
            "keepalive_age_ms=%llu keepalive_ok=%d "
            "keepalive_exception_count=%u keepalive_exception_age_ms=%llu "
            "token_refresh_exception_count=%u token_refresh_exception_age_ms=%llu "
            "drops_local=%u drops_sync=%u drops_queue=%u decode_errors=%u fd=%u",
            event,
            source ? source : "unknown",
            reason ? reason : "unknown",
            error_code,
            error_flags,
            active_loss_episode_id.load(),
            frame_width,
            frame_height,
            static_cast<unsigned long long>(pts_ns),
            static_cast<unsigned long long>(au_bytes),
            nal_types && *nal_types ? nal_types : "-",
            idr ? 1 : 0,
            static_cast<long long>(last_av_raw_delay_ns.load() / 1000),
            static_cast<long long>(last_av_policy_delay_ns.load() / 1000),
            static_cast<long long>(last_av_audio_age_ms.load()),
            last_av_using_audio_master.load() ? 1 : 0,
            static_cast<unsigned long long>(last_av_master_pts_ns.load()),
            video_queue_packets.load(),
            static_cast<unsigned long long>(video_queue_bytes.load()),
            video_queue_oldest_age_ms.load(),
            video_queue_high_watermark_packets.load(),
            static_cast<unsigned long long>(last_video_au_queue_age_us.load()),
            static_cast<unsigned long long>(last_decode_us.load()),
            static_cast<unsigned long long>(decode_max_us),
            static_cast<unsigned long long>(last_render_submit_us.load()),
            static_cast<unsigned long long>(render_max_us),
            static_cast<unsigned long long>(last_present_wait_us.load()),
            static_cast<unsigned long long>(present_wait_max_us),
            audio_latency_ms.load(),
            audio_buffer_ms.load(),
            audio_queued_buffers.load(),
            video_frames.load(),
            video_packets.load(),
            audio_packets.load(),
            rtp_video_packets.load(),
            rtp_video_missing_packets.load(),
            last_video_missing_delta.load(),
            last_video_gap_delta.load(),
            rtp_audio_missing_packets.load(),
            last_audio_missing_delta.load(),
            static_cast<unsigned long long>(video_jitter_us.load()),
            network_rtt_ms.load(),
            static_cast<unsigned long long>(last_webrtc_pump_gap_us.load()),
            static_cast<unsigned long long>(max_webrtc_pump_gap_us.load()),
            static_cast<unsigned long long>(last_webrtc_pump_duration_us.load()),
            static_cast<unsigned long long>(max_webrtc_pump_duration_us.load()),
            video_rtp_arrival_age_ms_diag.load(),
            video_rtp_last_arrival_gap_ms_diag.load(),
            video_rtp_max_arrival_gap_ms_diag.load(),
            video_rtp_highest_sequence_diag.load(),
            video_rtp_last_gap_packets_diag.load(),
            video_rtp_ssrc_diag.load(),
            video_rtp_ssrc_changes_diag.load(),
            video_rtp_nacks_diag.load(),
            video_rtp_resyncs_diag.load(),
            video_jitter_buffered_packets_diag.load(),
            video_jitter_buffered_frames_diag.load(),
            video_jitter_buffered_bytes_diag.load(),
            video_waiting_keyframe_diag.load() ? 1 : 0,
            corrupt,
            h264_overflow_frames.load(),
            h264_max_frame_bytes.load(),
            rtp_queue_high_watermark.load(),
            rtp_queue_drops.load(),
            rtp_queue_drop_delta_diag.load(),
            srtp_rtp_decrypt_failures.load(),
            keep_alive_interval_seconds.load(),
            last_keep_alive_duration_ms.load(),
            static_cast<unsigned long long>(keep_alive_age_ms),
            last_keep_alive_ok.load() ? 1 : 0,
            keep_alive_exception_count.load(),
            static_cast<unsigned long long>(keep_alive_exception_age_ms),
            token_refresh_exception_count.load(),
            static_cast<unsigned long long>(token_refresh_exception_age_ms),
            local_drops,
            video_sync_drops.load(),
            video_queue_drops.load(),
            decode_errors,
            local_drops + decode_errors + corrupt);
#else
        (void)source; (void)reason; (void)error_code; (void)error_flags;
        (void)frame_pts_ns; (void)access_unit_bytes;
        (void)frame_width; (void)frame_height; (void)nal_types; (void)is_idr;
#endif
    }
};

} // namespace lunar::stream
