#!/usr/bin/env python3
import re
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def method_body(source, signature):
    start = source.find(signature)
    require(start >= 0, f"Missing method: {signature}")
    brace = source.find("{", start)
    require(brace >= 0, f"Missing method body: {signature}")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise SystemExit(f"FAIL: Unterminated method body: {signature}")


def main():
    header = Path("src/stream/media_pipeline.h").read_text()
    impl = Path("src/stream/media_pipeline.cpp").read_text()

    require("#include <condition_variable>" in header,
            "MediaPipeline should own a condition variable for async media work")
    require("#include <deque>" in header,
            "MediaPipeline should own a bounded media packet queue")
    require("#include <thread>" in header,
            "MediaPipeline should own a worker thread")
    require("struct QueuedVideoPacket" in header,
            "MediaPipeline should store copied video packets")
    require("#include \"audio_packet_reorder.h\"" in header and
            "std::deque<EncodedAudioPacket>" in header,
            "MediaPipeline should store audio sequence and timestamp metadata")
    require("std::deque<QueuedDecodedAudio>" in header and
            "queued_decoded_audio_bytes_" in header,
            "Decoded PCM must have its own bounded worker queue")
    require("void videoWorkerLoop();" in header,
            "MediaPipeline should drain video on its own worker")
    require("void audioWorkerLoop();" in header,
            "MediaPipeline should drain audio on its own worker")
    require("bool enqueueVideoPacket(" in header,
            "decodeVideoPacket should enqueue copied video work")
    require("bool enqueueAudioPacket(" in header,
            "decodeAudioPacket should enqueue copied audio work")
    require("std::mutex video_queue_mutex_" in header,
            "Video queue must have an independent mutex")
    require("std::mutex audio_queue_mutex_" in header,
            "Audio queue must have an independent mutex")
    require("std::condition_variable video_queue_cv_" in header,
            "Video queue must have an independent condition variable")
    require("std::condition_variable audio_queue_cv_" in header,
            "Audio queue must have an independent condition variable")
    require("video_recovery_request_" in header and
            "video_decoder_reset_pending_" in header,
            "Video queue recovery must coordinate a decoder reset and keyframe request")
    require("recovery_epoch" in header and
            "video_waiting_for_keyframe_" in header and
            "contains_idr" in header,
            "Hard recovery must tag queued access units and gate decode on an IDR")
    require("hasVideoRecoveryRequest" in header and
            "clearVideoRecoveryRequest" in header,
            "MediaPipeline should expose throttled keyframe recovery state")

    video_body = method_body(
        impl,
        "bool MediaPipeline::decodeVideoPacket(const uint8_t* data, size_t len,")
    audio_body = method_body(
        impl,
        "bool MediaPipeline::decodeAudioPacket(const uint8_t* data, size_t len,")

    require("enqueueVideoPacket" in video_body,
            "decodeVideoPacket should enqueue the packet and return quickly")
    require("enqueueAudioPacket" in audio_body,
            "decodeAudioPacket should enqueue the packet and return quickly")
    require("video_decoder_->decode" not in video_body,
            "decodeVideoPacket must not synchronously call the video decoder")
    require("audio_decoder_->decode" not in audio_body,
            "decodeAudioPacket must not synchronously call the audio decoder")

    decoded_audio_body = method_body(
        impl, "bool MediaPipeline::playDecodedAudio(const AudioFrame& frame)")
    require("decoded_audio_queue_.push_back" in decoded_audio_body and
            "audio_player_->play" not in decoded_audio_body,
            "Decoded PCM callbacks must enqueue instead of touching Audren")

    require(re.search(r"video_worker_\s*=\s*std::thread", impl),
            "MediaPipeline should start a video worker")
    require(re.search(r"audio_worker_\s*=\s*std::thread", impl),
            "MediaPipeline should start an audio worker")
    require("video_worker_.join()" in impl,
            "MediaPipeline should join the video worker during shutdown")
    require("audio_worker_.join()" in impl,
            "MediaPipeline should join the audio worker during shutdown")
    require("video_queue_cv_.notify_one()" in impl,
            "MediaPipeline should wake the video worker after enqueue")
    require("audio_queue_cv_.notify_one()" in impl,
            "MediaPipeline should wake the audio worker after enqueue")
    require("video queue overflow" in impl and
            "beginHardVideoRecoveryLocked" in impl,
            "Video queue overflow should trigger reference-chain recovery")
    require("resetForKeyframe" in impl,
            "Video worker should reset the decoder after media discontinuity")
    hard_recovery = method_body(
        impl,
        "bool MediaPipeline::beginHardVideoRecoveryLocked(bool force_new_epoch,")
    require("video_queue_.clear()" in hard_recovery and
            ("video_recovery_epoch_.fetch_add(1)" in hard_recovery or
             "applyBoundedVideoRecovery" in impl) and
            ("video_waiting_for_keyframe_ = true" in hard_recovery or
             "video_waiting_for_keyframe_ = recovery_state.waiting_for_keyframe"
             in hard_recovery),
            "Hard recovery must discard stale queued video before advancing the epoch")
    worker = method_body(impl, "void MediaPipeline::videoWorkerLoop()")
    require("resetVideoDecoderForKeyframe()" in worker and
            "packet.contains_idr" in worker,
            "Only the video worker may reset the decoder before a recovery IDR")
    process_video = method_body(
        impl, "void MediaPipeline::processVideoPacket(const QueuedVideoPacket& packet)")
    require("boundedVideoPacketIsCurrent" in process_video and
            "boundedVideoMayDecodeWhileRecovering" in process_video,
            "Stale epochs and non-IDR recovery packets must not reach the decoder")
    require('beginHardVideoRecovery("video decoder error", true)' in process_video,
            "A recovery IDR decode failure must force a fresh decoder reset epoch")
    require("pre_copy_admission" in impl and
            impl.index("pre_copy_admission") <
            impl.index("packet.data.assign(data, data + len);"),
            "Bounded admission must reject known drops before copying the AU")
    require("std::this_thread::sleep_for(std::chrono::nanoseconds(wait_ns))" not in impl,
            "The video decode worker should not sleep for A/V lead time")

    audio_handler = method_body(
        impl,
        "void MediaPipeline::handleAudioFrame(const AudioFrame& frame,")
    require("audio_player_->play(frame)" in audio_handler,
            "Audio frames should be submitted on the audio worker")
    require("lifecycle_mutex_" not in audio_handler,
            "Blocking audren writes must not hold the lifecycle mutex")
    submit_decoded = method_body(
        impl, "bool MediaPipeline::submitDecodedAudio(const AudioFrame& frame,")
    require("audio_player_->play(frame)" in submit_decoded,
            "Only the media audio worker should submit decoded PCM")

    initialize_body = method_body(
        impl,
        "bool MediaPipeline::initialize(int width, int height, PerfStats* perf,")
    shutdown_body = method_body(impl, "void MediaPipeline::shutdown()")
    start_workers_body = method_body(
        impl,
        "bool MediaPipeline::startWorkers(uint32_t generation)")
    require("running_ = false" in initialize_body,
            "Reinitialize must reject callbacks before joining old workers")
    require("running_ = false" in shutdown_body,
            "Shutdown must reject callbacks before joining workers")
    require(initialize_body.index("running_ = false") < initialize_body.index("stopWorkers()"),
            "Reinitialize must reject callbacks before joining old workers")
    require(shutdown_body.index("running_ = false") < shutdown_body.index("stopWorkers()"),
            "Shutdown must reject callbacks before joining workers")
    require(start_workers_body.index("stopWorkers()") > start_workers_body.index("try {"),
            "startWorkers may stop only when thread creation fails")

    print("MediaPipeline async tests passed")


if __name__ == "__main__":
    main()
