# Audio And Input Reliability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Xbox controller packets, vibration feedback, Opus packet-loss handling, audio playback scheduling, and A/V synchronization reliable enough for Ryubing regression tests and real Switch validation.

**Architecture:** Keep Xbox protocol handling in the existing session and libpeer boundaries, but expose RTP sequence/timestamp metadata instead of replacing it with callback-arrival time. Video remains on the current media worker; audio gets a bounded queue, deterministic reorder/PLC stage, and dedicated worker. Audio output remains the existing Moonlight-aligned `libopus + audren` path and becomes the master clock used to schedule video.

**Tech Stack:** C++17, C99 legacy libpeer, libopus multistream, libnx audren/HID, Borealis, Python `unittest`, Docker `devkitpro/devkita64:20251117`, Ryubing Canary 1.3.333.

## Global Constraints

- Do not build Switch-targeted libraries or the Switch NRO on macOS.
- Build Switch artifacts only in Docker with `devkitpro/devkita64:20251117`.
- Keep `WEBRTC_PROVIDER=legacy`; do not switch to upstream libpeer.
- Preserve `CONFIG_PACKET_BUFFER_SIZE`; do not replace it with `CONFIG_MTU` for inbound RTP.
- Reproduce every active `lib/libpeer` source change in `tools/libpeer_legacy/legacy-libpeer-switch.patch`.
- Keep app startup light; these changes are initialized only after the user starts a stream.
- Do not commit tokens, simulator data, logs, NRO files, or generated build outputs.
- Use a four-packet Opus reorder window, at most three consecutive PLC frames, and reset on forward sequence jumps larger than 64 packets.
- Bound the dedicated audio queue to 512 packets and 4 MiB.
- Real Switch hardware remains the final proof for audible output, controller direction, rumble routing, and long-run A/V sync.

---

### Task 1: Correct Xbox Gamepad Encoding And Session Lifecycle

**Files:**
- Create: `tests/xinput_encoder_test.cpp`
- Create: `tests/input_lifecycle_test.py`
- Modify: `Makefile.desktop`
- Modify: `src/input/xinput_encoder.h`
- Modify: `src/input/xinput_encoder.cpp`
- Modify: `src/input/gamepad_reader.cpp`
- Modify: `src/app/xbox_stream_session.cpp`
- Test: `tests/xbox_stream_session_order_test.py`

**Interfaces:**
- Produces: `void lunar::input::XInputEncoder::reset()`.
- Produces: `XInputEncoder::encode(const GamepadState&)` always returns one 38-byte report.
- Produces: session metadata sequence `0`, followed by gamepad sequence `1`, after start or reconnect.

- [ ] **Step 1: Add exact wire-format tests and the desktop test target**

Add `tests/xinput_encoder_test.cpp` with little-endian readers and these assertions:

```cpp
#include "input/xinput_encoder.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>

using lunar::input::GamepadState;
using lunar::input::XInputEncoder;

static uint16_t u16(const std::vector<uint8_t>& packet, size_t offset) {
    return static_cast<uint16_t>(packet[offset]) |
           static_cast<uint16_t>(packet[offset + 1] << 8);
}

static int16_t s16(const std::vector<uint8_t>& packet, size_t offset) {
    return static_cast<int16_t>(u16(packet, offset));
}

static uint32_t u32(const std::vector<uint8_t>& packet, size_t offset) {
    return static_cast<uint32_t>(packet[offset]) |
           (static_cast<uint32_t>(packet[offset + 1]) << 8) |
           (static_cast<uint32_t>(packet[offset + 2]) << 16) |
           (static_cast<uint32_t>(packet[offset + 3]) << 24);
}

int main() {
    XInputEncoder encoder;
    GamepadState neutral;
    auto first = encoder.encode(neutral);
    auto repeated = encoder.encode(neutral);
    assert(first.size() == 38);
    assert(repeated.size() == 38);
    assert(u16(first, 0) == 2);
    assert(u32(first, 2) == 0);
    assert(u32(repeated, 2) == 1);
    assert(u32(first, 30) == 0);
    assert(u32(first, 34) == 0);

    GamepadState a;
    a.a = true;
    auto a_packet = encoder.encode(a);
    assert(u16(a_packet, 16) == 0x0010);
    assert(u32(a_packet, 30) == 0x00001000);

    GamepadState left_up;
    left_up.left_stick_y = 32767;
    auto stick_packet = encoder.encode(left_up);
    assert(s16(stick_packet, 20) == 32767);
    assert(u32(stick_packet, 30) == 0x000c0000);

    GamepadState trigger;
    trigger.left_trigger = 65535;
    auto trigger_packet = encoder.encode(trigger);
    assert(u16(trigger_packet, 26) == 65535);
    assert(u32(trigger_packet, 30) == 0x00010000);

    encoder.reset();
    auto metadata = encoder.encodeMetadata(0);
    auto after_reset = encoder.encode(neutral);
    assert(u32(metadata, 2) == 0);
    assert(u32(after_reset, 2) == 1);
    return 0;
}
```

Add an `input_tests` target to `Makefile.desktop` that compiles the test with `src/input/xinput_encoder.cpp` and runs `build/pc/xinput_encoder_test`.

- [ ] **Step 2: Run the input test and verify RED**

Run:

```sh
make -f Makefile.desktop input_tests
```

Expected: compilation fails because `reset()` does not exist, or the old encoder fails repeated-state, physicality, and positive-up Y assertions.

- [ ] **Step 3: Implement the exact XStreaming packet contract**

In `src/input/xinput_encoder.h`, remove the cached state and add:

```cpp
void reset();
```

In `src/input/xinput_encoder.cpp`:

```cpp
void XInputEncoder::reset() {
    sequence_ = 0;
}
```

Use `std::chrono::steady_clock` in `write_timestamp()`. Remove redundant-state suppression. Replace the current full-controller physicality mask with a physicality enum matching XStreaming and OR only active buttons, triggers, and both axis bits for each non-idle stick. Write `state.left_stick_y` and `state.right_stick_y` directly without negation.

- [ ] **Step 4: Make gamepad initialization idempotent and session reset ordering explicit**

Add `tests/input_lifecycle_test.py` that requires:

```python
reader = Path("src/input/gamepad_reader.cpp").read_text()
session = Path("src/app/xbox_stream_session.cpp").read_text()
require("delete static_cast<PadState*>(pad_state_);" in reader,
        "initialize must delete an old PadState")
require("pad_state_ = nullptr;" in reader,
        "initialize must clear the old PadState pointer")
require(session.index("xinput_.reset();") < session.index("encodeMetadata(0)"),
        "encoder reset must happen before metadata")
```

Change `GamepadReader::initialize()` to set `initialized_ = false`, delete and clear an existing Switch `PadState`, allocate with `new (std::nothrow)`, return `false` on allocation failure, then configure and initialize libnx input.

Call `xinput_.reset()` once before starting the stream thread. On successful reconnect, call it after data channels are ready and immediately before setting `control_started = false`, so no unsent polling packet consumes sequence zero before metadata.

Send the current gamepad packet every 16ms whenever control is started and the transport is connected. Record `PerfStats::input_packets` only after a successful `sendInputPacket`; log a bounded diagnostic on failure and retry naturally on the next loop.

- [ ] **Step 5: Run focused input tests and verify GREEN**

Run:

```sh
make -f Makefile.desktop input_tests
python3 tests/input_lifecycle_test.py
python3 tests/xbox_stream_session_order_test.py
```

Expected: all three commands pass.

- [ ] **Step 6: Commit the input fix**

```sh
git add Makefile.desktop tests/xinput_encoder_test.cpp tests/input_lifecycle_test.py src/input/xinput_encoder.h src/input/xinput_encoder.cpp src/input/gamepad_reader.cpp src/app/xbox_stream_session.cpp tests/xbox_stream_session_order_test.py
git commit -m "fix: align Xbox controller input packets"
```

---

### Task 2: Parse Xbox Vibration Reports And Reinitialize HID Safely

**Files:**
- Create: `src/webrtc/xbox_input_feedback.h`
- Create: `src/webrtc/xbox_input_feedback.cpp`
- Create: `tests/xbox_input_feedback_test.cpp`
- Modify: `Makefile.desktop`
- Modify: `Makefile.switch`
- Modify: `src/webrtc/peer_manager.cpp`
- Modify: `src/input/rumble_controller.cpp`
- Modify: `src/input/rumble_controller.h`
- Modify: `tests/input_lifecycle_test.py`

**Interfaces:**
- Produces: `bool parseXboxVibrationPacket(const uint8_t*, size_t, XboxVibrationCommand&)`.
- Consumes: XStreaming section order: optional `ServerMetadata` then one 11-byte vibration section.

- [ ] **Step 1: Add vibration parser tests**

Create `tests/xbox_input_feedback_test.cpp` with three packets:

```cpp
#include "webrtc/xbox_input_feedback.h"
#include <cassert>
#include <cstdint>

using lunar::webrtc::XboxVibrationCommand;
using lunar::webrtc::parseXboxVibrationPacket;

int main() {
    const uint8_t vibration[] = {
        0x80, 0x00,
        0x00, 0x00, 25, 50, 75, 100,
        0xfa, 0x00, 0x0a, 0x00, 0x02,
    };
    XboxVibrationCommand command;
    assert(parseXboxVibrationPacket(vibration, sizeof(vibration), command));
    assert(command.left_motor == 0.25f);
    assert(command.right_motor == 0.50f);
    assert(command.left_trigger == 0.75f);
    assert(command.right_trigger == 1.00f);
    assert(command.duration_ms == 250);
    assert(command.delay_ms == 10);
    assert(command.repeat == 2);

    const uint8_t metadata_and_vibration[] = {
        0x90, 0x00,
        0xd0, 0x02, 0x00, 0x00,
        0x00, 0x05, 0x00, 0x00,
        0x00, 0x00, 10, 20, 30, 40,
        0x14, 0x00, 0x00, 0x00, 0x00,
    };
    assert(parseXboxVibrationPacket(metadata_and_vibration,
                                    sizeof(metadata_and_vibration), command));

    const uint8_t unsupported_mixed[] = {0x82, 0x00, 0x00, 0x00};
    assert(!parseXboxVibrationPacket(unsupported_mixed,
                                     sizeof(unsupported_mixed), command));
    assert(!parseXboxVibrationPacket(vibration, sizeof(vibration) - 1, command));
    return 0;
}
```

Add a `feedback_tests` target to `Makefile.desktop` and include `src/webrtc/xbox_input_feedback.cpp` in desktop and Switch application sources.

- [ ] **Step 2: Run the parser test and verify RED**

Run:

```sh
make -f Makefile.desktop feedback_tests
```

Expected: compilation fails because the parser header and implementation do not exist.

- [ ] **Step 3: Implement the bounded parser and use it from PeerManager**

Define:

```cpp
struct XboxVibrationCommand {
    float left_motor = 0.0f;
    float right_motor = 0.0f;
    float left_trigger = 0.0f;
    float right_trigger = 0.0f;
    uint16_t duration_ms = 0;
    uint16_t delay_ms = 0;
    uint8_t repeat = 0;
};
```

The parser must require the vibration bit, reject report bits other than `0x10` and `0x80`, skip exactly eight metadata bytes when `0x10` is present, require 11 remaining vibration bytes, and decode motor percentages, duration, delay, and repeat with little-endian reads.

Replace the guessed bit-loop in `PeerManager::onDataChannelMessage()` with this parser. Invoke `callbacks_.on_rumble` only on success. For an invalid vibration report, emit a bounded `lunar::diagnosticLog` entry and do not forward the payload to another channel handler.

- [ ] **Step 4: Make rumble initialization repeatable**

At the beginning of `RumbleController::initialize()`, call `stop()`, set `hid_rumble_initialized_ = false`, set `vibration_handle_count_ = 0`, and zero `vibration_handles_`. Probe handheld handles first, then player-one JoyDual handles. Replace runtime `fprintf` diagnostics in this path with `lunar::diagnosticLog`.

Extend `tests/input_lifecycle_test.py` to assert that `stop()` and handle resets appear before the first `hidInitializeVibrationDevices()` call.

- [ ] **Step 5: Run vibration and lifecycle tests**

Run:

```sh
make -f Makefile.desktop feedback_tests
python3 tests/input_lifecycle_test.py
python3 tests/datachannel_ppid_test.py
```

Expected: all commands pass.

- [ ] **Step 6: Commit vibration handling**

```sh
git add Makefile.desktop Makefile.switch src/webrtc/xbox_input_feedback.h src/webrtc/xbox_input_feedback.cpp tests/xbox_input_feedback_test.cpp src/webrtc/peer_manager.cpp src/input/rumble_controller.cpp src/input/rumble_controller.h tests/input_lifecycle_test.py
git commit -m "fix: align Xbox vibration handling"
```

---

### Task 3: Propagate RTP Sequence And Timestamp Metadata

**Files:**
- Create: `src/webrtc/rtp_clock_mapper.h`
- Create: `tests/rtp_clock_mapper_test.cpp`
- Modify: `tests/h264_rtp_depacketizer_test.c`
- Modify: `Makefile.desktop`
- Modify: `lib/libpeer/src/rtp.h` (ignored working checkout)
- Modify: `lib/libpeer/src/rtp.c` (ignored working checkout)
- Modify: `lib/libpeer/src/peer_connection.h` (ignored working checkout)
- Modify: `lib/libpeer/src/peer_connection.c` (ignored working checkout)
- Modify: `src/webrtc/peer_manager.h`
- Modify: `src/webrtc/peer_manager.cpp`
- Modify: `src/app/xbox_stream_session.cpp`
- Modify: `tools/libpeer_legacy/legacy-libpeer-switch.patch`
- Modify: `tools/libpeer_legacy/README.md`

**Interfaces:**
- Produces legacy libpeer callback: `RtpOnPacket(uint8_t*, size_t, uint16_t, uint32_t, void*)`.
- Produces Lunar callback: `MediaFrameCallback(const uint8_t*, size_t, uint16_t, uint64_t)`.
- Produces: `RtpClockMapper(uint32_t clock_rate)`, `reset()`, and `map(uint32_t rtp_timestamp, uint64_t arrival_ns)`.

- [ ] **Step 1: Extend RTP depacketizer tests before changing libpeer**

Change the test callback in `tests/h264_rtp_depacketizer_test.c` to capture payload, sequence, and timestamp. Add a generic Opus case that builds a 12-byte RTP header with sequence `0x1234`, timestamp `0x01020304`, and a three-byte payload, then asserts the callback receives exactly those values. Add an H.264 fragmented-frame assertion that the callback timestamp equals the frame RTP timestamp and sequence equals the final contributing packet.

- [ ] **Step 2: Run the RTP test and verify RED**

Run:

```sh
make -f Makefile.desktop h264_rtp_tests
```

Expected: compilation fails against the old three-argument `RtpOnPacket`, proving metadata is not exposed.

- [ ] **Step 3: Update legacy libpeer callback propagation**

Change `RtpOnPacket` to:

```c
typedef void (*RtpOnPacket)(uint8_t* packet,
                            size_t bytes,
                            uint16_t sequence,
                            uint32_t timestamp,
                            void* user_data);
```

For generic decoding, parse sequence and timestamp once and pass them to the callback. For H.264, add `h264_frame_last_seq` to `RtpDecoder`, update it only after a packet has been assigned to the current frame, and pass `h264_frame_last_seq` plus `h264_frame_timestamp` from `h264_flush_frame()`. Update encoder callbacks to pass the sequence/timestamp encoded in their generated RTP header. Update `peer_connection_outgoing_rtp_packet`, `PeerConfiguration::onaudiotrack`, and `PeerConfiguration::onvideotrack` signatures accordingly.

- [ ] **Step 4: Add and test wrap-safe RTP clock mapping**

Create a header-only `RtpClockMapper` using a track-local first-packet anchor and signed 32-bit RTP deltas:

```cpp
class RtpClockMapper {
public:
    explicit RtpClockMapper(uint32_t clock_rate) : clock_rate_(clock_rate) {}
    void reset();
    uint64_t map(uint32_t rtp_timestamp, uint64_t arrival_ns);
private:
    uint32_t clock_rate_;
    bool anchored_ = false;
    uint32_t anchor_rtp_ = 0;
    uint64_t anchor_ns_ = 0;
};
```

`tests/rtp_clock_mapper_test.cpp` must verify 90kHz video conversion, 48kHz audio conversion, wrap from `0xfffffff0` to `0x00000020`, and preservation of different first-arrival anchors between audio and video.

Add `rtp_clock_tests` to `Makefile.desktop`, run it before implementation to see the missing-header failure, then implement until it passes.

- [ ] **Step 5: Wire mapped timestamps through PeerManager**

Change `PeerCallbacks` to use:

```cpp
using MediaFrameCallback =
    std::function<void(const uint8_t*, size_t, uint16_t, uint64_t)>;
```

Store `RtpClockMapper video_clock_{90000}` and `audio_clock_{48000}` in `PeerManager`. Reset both in `initialize()`. In each libpeer callback, calculate one `arrival_ns = elapsedNs(media_clock_start_)`, map the RTP timestamp, and invoke the application callback with sequence and mapped nanoseconds. Update the session lambdas to accept sequence; video may ignore it at this stage, and audio passes it in Task 5.

- [ ] **Step 6: Regenerate the reproducible legacy patch**

Regenerate `tools/libpeer_legacy/legacy-libpeer-switch.patch` from the pinned base commit so it contains all existing legacy changes plus the RTP callback changes:

```sh
git -C lib/libpeer diff bdc50f0cae13f19a31bb11827daea3a8354b173f -- src > tools/libpeer_legacy/legacy-libpeer-switch.patch
```

Update the README to state that decoded callbacks expose RTP sequence/timestamp metadata. Verify the patch applies to a clean checkout with:

```sh
tmpdir=$(mktemp -d)
git clone --no-checkout lib/libpeer "$tmpdir/libpeer"
git -C "$tmpdir/libpeer" checkout bdc50f0cae13f19a31bb11827daea3a8354b173f
git -C "$tmpdir/libpeer" apply "$PWD/tools/libpeer_legacy/legacy-libpeer-switch.patch"
```

Expected: `git apply` exits zero.

- [ ] **Step 7: Run RTP and callback regression tests**

Run:

```sh
make -f Makefile.desktop h264_rtp_tests rtp_clock_tests
python3 tests/libpeer_agent_recv_test.py
python3 tests/libpeer_dtls_read_loop_test.py
python3 tests/libpeer_sctp_config_test.py
python3 tests/xbox_stream_session_order_test.py
```

Expected: all commands pass.

- [ ] **Step 8: Commit RTP metadata propagation**

```sh
git add Makefile.desktop tests/h264_rtp_depacketizer_test.c tests/rtp_clock_mapper_test.cpp src/webrtc/rtp_clock_mapper.h src/webrtc/peer_manager.h src/webrtc/peer_manager.cpp src/app/xbox_stream_session.cpp tools/libpeer_legacy/legacy-libpeer-switch.patch tools/libpeer_legacy/README.md
git commit -m "fix: preserve RTP media timestamps"
```

---

### Task 4: Add Opus Reordering And Packet-Loss Concealment

**Files:**
- Create: `src/stream/audio_packet_reorder.h`
- Create: `src/stream/audio_packet_reorder.cpp`
- Create: `tests/audio_packet_reorder_test.cpp`
- Create: `tests/audio_decoder_plc_test.cpp`
- Modify: `Makefile.desktop`
- Modify: `Makefile.switch`
- Modify: `src/stream/audio_decoder.h`
- Modify: `src/stream/audio_decoder.cpp`

**Interfaces:**
- Produces: `AudioPacketReorder::push(EncodedAudioPacket) -> std::vector<AudioReorderAction>`.
- Produces: `AudioDecoder::decodeMissing(uint64_t timestamp)`.
- Produces: `AudioDecoder::lastFrameSamples() const`.

- [ ] **Step 1: Add deterministic reorder tests**

Define tests for these exact sequences:

```cpp
// In order: 10 -> Packet(10)
// Reordered: 12,13,14 then 11 -> Packet(11), Packet(12), Packet(13), Packet(14)
// One lost packet: after 10, receive 12,13,14,15 -> Missing(11), then Packet(12..15)
// Late packet: after 10,11, receive 10 -> no actions
// Wrap: after 65535, receive 0 -> Packet(0)
// Large jump: after 10, receive 100 -> Packet(100), no Missing actions
```

Each `EncodedAudioPacket` contains payload, sequence, mapped timestamp, and generation. `AudioReorderAction` has `Type::Packet` or `Type::Missing` plus the affected sequence; packet actions own the packet payload.

- [ ] **Step 2: Run reorder tests and verify RED**

Add an `audio_reorder_tests` target and run:

```sh
make -f Makefile.desktop audio_reorder_tests
```

Expected: compilation fails because the reorder component does not exist.

- [ ] **Step 3: Implement the four-packet reorder state machine**

Use wrap-safe signed sequence distance. Decode the expected packet immediately. Retain up to four future packets. When four future packets are buffered, find the nearest forward sequence, emit up to three `Missing` actions, skip any additional missing sequence numbers, then drain contiguous buffered packets. Discard duplicates and older packets. A forward jump larger than 64 clears pending state, resets expected sequence to the new packet, and emits that packet without PLC.

- [ ] **Step 4: Add Opus PLC tests before implementation**

Create `tests/audio_decoder_plc_test.cpp` using the valid 20ms Opus silence packet `{0xf8, 0xff, 0xfe}`. Capture decoded frames and assert:

```cpp
assert(decoder.initialize());
assert(decoder.decode(packet, sizeof(packet), 1'000'000'000ULL));
assert(frames.back().sample_count == 960);
assert(decoder.lastFrameSamples() == 960);
assert(decoder.decodeMissing(1'020'000'000ULL));
assert(frames.back().sample_count == 960);
assert(frames.back().timestamp == 1'020'000'000ULL);
```

Add `audio_decoder_tests` to `Makefile.desktop`, linking `src/stream/audio_decoder.cpp` and `-lopus`.

- [ ] **Step 5: Run PLC tests and verify RED**

Run:

```sh
make -f Makefile.desktop audio_decoder_tests
```

Expected: compilation fails because `decodeMissing()` and `lastFrameSamples()` do not exist.

- [ ] **Step 6: Implement Opus duration tracking and PLC**

Refactor normal decode and PLC through one internal function. For normal packets, call `opus_packet_get_nb_samples(data, len, 48000)` and remember the successful decoded sample count. For PLC, call `opus_multistream_decode(decoder, nullptr, 0, pcm, last_frame_samples_, 0)`. Default `last_frame_samples_` to 960 before the first valid packet. Emit the same `AudioFrame` callback for normal and PLC output; return `false` and record a drop only when Opus returns an error.

- [ ] **Step 7: Run audio unit tests**

Run:

```sh
make -f Makefile.desktop audio_reorder_tests audio_decoder_tests
python3 tests/audio_decoder_config_test.py
```

Expected: all commands pass.

- [ ] **Step 8: Commit reorder and PLC support**

```sh
git add Makefile.desktop Makefile.switch src/stream/audio_packet_reorder.h src/stream/audio_packet_reorder.cpp tests/audio_packet_reorder_test.cpp tests/audio_decoder_plc_test.cpp src/stream/audio_decoder.h src/stream/audio_decoder.cpp
git commit -m "fix: conceal missing Opus packets"
```

---

### Task 5: Split Audio Processing And Use Audren Playback As Master Clock

**Files:**
- Create: `src/stream/audio_timing.h`
- Create: `tests/audio_timing_test.cpp`
- Create: `tests/av_sync_test.cpp`
- Modify: `Makefile.desktop`
- Modify: `src/stream/media_pipeline.h`
- Modify: `src/stream/media_pipeline.cpp`
- Modify: `src/stream/audio_player.h`
- Modify: `src/stream/audio_player.cpp`
- Modify: `src/stream/av_sync.h`
- Modify: `src/stream/av_sync.cpp`
- Modify: `src/app/xbox_stream_session.cpp`
- Modify: `tests/media_pipeline_async_test.py`

**Interfaces:**
- Changes: `MediaPipeline::decodeAudioPacket(const uint8_t*, size_t, uint16_t sequence, uint64_t timestamp)`.
- Produces: `size_t AudioPlayer::queuedSampleCount()`.
- Produces: `estimateAudioPlaybackTimestamp(frame_start_ns, frame_samples, sample_rate, queued_samples)`.

- [ ] **Step 1: Strengthen the async worker regression and verify RED**

Change `tests/media_pipeline_async_test.py` to require separate `video_worker_` and `audio_worker_`, separate queue mutexes/condition variables, a `QueuedAudioPacket` carrying sequence, and both worker joins during shutdown. Require `handleAudioFrame()` not to hold `lifecycle_mutex_` while calling `audio_player_->play()`.

Run:

```sh
python3 tests/media_pipeline_async_test.py
```

Expected: failure because only one shared media worker exists.

- [ ] **Step 2: Add playback timestamp and A/V clock tests**

Create `audio_timing.h` with a pure helper and test:

```cpp
assert(estimateAudioPlaybackTimestamp(
    1'000'000'000ULL, 960, 48000, 960) == 1'000'000'000ULL);
assert(estimateAudioPlaybackTimestamp(
    1'000'000'000ULL, 960, 48000, 480) == 1'010'000'000ULL);
assert(estimateAudioPlaybackTimestamp(
    10'000'000ULL, 960, 48000, 9600) == 0);
```

Create `tests/av_sync_test.cpp` that starts `AVSync`, sets video PTS to `1,000,000,000ns`, sets audio playback PTS to `900,000,000ns`, and asserts `getVideoDelayNs(1,000,000,000ns)` is `100,000,000ns`. This fails under the current independent first-audio/first-video subtraction.

Add `audio_timing_tests` and `av_sync_tests` to `Makefile.desktop` and run both to verify RED.

- [ ] **Step 3: Expose queued audio samples**

Add `AudioPlayer::queuedSampleCount()`. On Switch, lock `mutex_`, update audren, read `audrvVoiceGetPlayedSampleCount()`, and return `max(total_queued_samples_ - played_samples, 0)`. On desktop, convert `SDL_GetQueuedAudioSize()` bytes into per-channel PCM sample frames. Do not hold `lifecycle_mutex_` while querying or waiting on audren.

- [ ] **Step 4: Split video and audio queue ownership**

Keep the current queue/thread as video-only and add:

```cpp
std::mutex audio_queue_mutex_;
std::condition_variable audio_queue_cv_;
std::deque<EncodedAudioPacket> audio_queue_;
std::thread audio_worker_;
bool audio_worker_stop_ = false;
size_t queued_audio_bytes_ = 0;
```

`decodeVideoPacket()` continues copying to the video queue. `decodeAudioPacket()` copies payload plus sequence/timestamp/generation into the audio queue, drops the oldest packet while either 512 packets or 4 MiB would be exceeded, records `PerfStats::audio_drops`, and returns immediately.

Start both workers only after all media components initialize. During shutdown, set both stop flags, notify both condition variables, join both workers, then acquire `lifecycle_mutex_` and destroy decoders, renderers, AV sync, and audren resources.

- [ ] **Step 5: Process reorder actions and PLC on the audio worker**

The audio worker owns one `AudioPacketReorder` per generation. For `Packet` actions, call `audio_decoder_->decode(payload, size, mapped_timestamp)`. For `Missing` actions, call `decodeMissing(last_decoded_audio_end_timestamp_)`, capped by the reorder component. Update `last_decoded_audio_end_timestamp_` from every successfully decoded `AudioFrame`, even when playback later drops it, so PLC timestamps stay continuous.

Because workers are joined before component destruction, remove `lifecycle_mutex_` from the blocking decode/play callback path. Keep generation checks before using component pointers.

- [ ] **Step 6: Update the audio master clock and AVSync comparison**

After `audio_player_->play(frame)` returns `true`, calculate:

```cpp
const uint64_t playback_pts = estimateAudioPlaybackTimestamp(
    frame.timestamp,
    frame.sample_count,
    frame.sample_rate,
    audio_player_->queuedSampleCount());
av_sync_->updateAudioPts(playback_pts);
```

Do not update AV sync after a failed write.

Change `AVSync` to compare absolute mapped media timestamps. With fresh audio, `master_pts = audio_pts_`. Without fresh audio, use `first_video_pts_ + elapsed_since_first_video_wall_time`. Remove subtraction of `first_audio_pts_` and `first_video_pts_` from the audio-master path. Keep the 500ms stale-audio fallback and the existing +/-200ms delay clamp.

- [ ] **Step 7: Pass audio sequence into the pipeline and run focused tests**

Update `XboxStreamSession::createPeerCallbacks()` so audio calls:

```cpp
media_.decodeAudioPacket(data, len, sequence, timestamp);
```

Run:

```sh
python3 tests/media_pipeline_async_test.py
make -f Makefile.desktop audio_timing_tests av_sync_tests audio_reorder_tests audio_decoder_tests
make -f Makefile.desktop stream_tests
python3 tests/xbox_stream_session_order_test.py
```

Expected: all commands pass and desktop stream tests remain green.

- [ ] **Step 8: Commit independent audio processing and clocking**

```sh
git add Makefile.desktop src/stream/audio_timing.h tests/audio_timing_test.cpp tests/av_sync_test.cpp src/stream/media_pipeline.h src/stream/media_pipeline.cpp src/stream/audio_player.h src/stream/audio_player.cpp src/stream/av_sync.h src/stream/av_sync.cpp src/app/xbox_stream_session.cpp tests/media_pipeline_async_test.py
git commit -m "fix: isolate audio playback and AV sync"
```

---

### Task 6: Validate Input And Exercise Rumble In The Mock Xbox Server

**Files:**
- Modify: `tools/mock_xbox/mock_xbox_server.py`
- Modify: `tools/mock_xbox/test_protocol_shapes.py`
- Modify: `docs/ryujinx_testing.md`

**Interfaces:**
- Produces: `parse_input_packet(bytes) -> dict`.
- Produces: `InputPacketValidator.validate(bytes) -> dict` with strict sequence progression.
- Produces CLI option: `--send-test-rumble`, default disabled.

- [ ] **Step 1: Add failing mock protocol tests**

Add tests that construct exact ClientMetadata and gamepad reports, then assert parsed sequence, button mask, signed Y axis, triggers, physical/virtual physicality, and exact report length. Add invalid cases for wrong frame count, wrong physicality, duplicate sequence, and truncated payload. Add a test for `build_test_rumble_packet()` requiring report type `0x90`, server height `720`, server width `1280`, and the exact 11-byte vibration payload.

- [ ] **Step 2: Run mock tests and verify RED**

Run:

```sh
python3 tools/mock_xbox/test_protocol_shapes.py
```

Expected: import or assertion failure because parser, validator, and rumble builder do not exist.

- [ ] **Step 3: Implement strict input validation**

Parse the 14-byte header with `<HId`. Accept ClientMetadata only at exactly 15 bytes. Accept Gamepad only at exactly 38 bytes with frame count one. Calculate expected physicality from active button-mask bits, nonzero triggers, and nonzero sticks; require virtual physicality zero. `InputPacketValidator` tracks the last sequence and requires `(last + 1) & 0xffffffff`.

In `_setup_input_channel`, create one validator per channel. Log valid gamepad reports at bounded debug frequency. Log malformed packets as errors without terminating the WebRTC connection.

- [ ] **Step 4: Add optional deterministic rumble**

Add global `_send_test_rumble` from `--send-test-rumble`. After the first valid gamepad packet, send one combined ServerMetadata + Vibration report:

```python
struct.pack("<HII", 0x90, 720, 1280) +
struct.pack("<BBBBBBHHB", 0, 0, 25, 50, 75, 100, 250, 0, 0)
```

Do not send it when the flag is absent. Update `docs/ryujinx_testing.md` with the flag and the expected LunarNX diagnostic line.

- [ ] **Step 5: Run mock protocol regressions**

Run:

```sh
python3 tools/mock_xbox/test_protocol_shapes.py
```

Expected: all protocol-shape tests pass. The live mock integration is exercised in Task 7.

- [ ] **Step 6: Commit mock validation**

```sh
git add tools/mock_xbox/mock_xbox_server.py tools/mock_xbox/test_protocol_shapes.py docs/ryujinx_testing.md
git commit -m "test: validate Xbox input and rumble packets"
```

---

### Task 7: Full Regression, Docker Build, And Ryubing Mock Smoke Test

**Files:**
- Test: `build/switch/LunarNX.nro`
- Test: `$HOME/work/self/ryujinx-data/sdcard/switch/LunarNX/lunarnx.log`

**Interfaces:**
- Consumes all prior task outputs.
- Produces a Docker-built `build/switch/LunarNX.nro` for local testing only; it remains uncommitted.

- [ ] **Step 1: Run all focused regressions**

```sh
make -f Makefile.desktop input_tests feedback_tests h264_rtp_tests rtp_clock_tests audio_reorder_tests audio_decoder_tests audio_timing_tests av_sync_tests stream_tests
python3 tests/xbox_stream_session_order_test.py
python3 tests/libpeer_sctp_config_test.py
python3 tests/libpeer_dtls_read_loop_test.py
python3 tests/datachannel_ppid_test.py
python3 tests/media_pipeline_async_test.py
python3 tools/mock_xbox/test_protocol_shapes.py
git diff --check
```

Expected: every command passes and `git diff --check` prints nothing.

- [ ] **Step 2: Clean-build the Switch NRO inside Docker**

Run:

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    set -e
    export DEVKITPRO=/opt/devkitpro
    export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
    make -f Makefile.switch clean
    make -f Makefile.switch -j$(nproc) \
      NETWORK_DIAG=0 CURL_PROVIDER=wiliwili CURL_VERIFY=0 \
      CURL_VERBOSE=0 CURL_TIMEOUT_MS=30000
  '
```

Expected: `build/switch/LunarNX.nro` is produced without using any macOS Switch-target library.

- [ ] **Step 3: Run a Ryubing mock-stream smoke test**

Start the mock server:

```sh
python3 tools/mock_xbox/mock_xbox_server.py \
  --video /tmp/lunarnx_test_720p60.mp4 \
  --public-ip 192.168.9.226 \
  --http-port 8080 \
  --send-test-rumble
```

Launch LunarNX without raw simulator logging:

```sh
./scripts/run_ryubing_nro.sh build/switch/LunarNX.nro
```

Manually start the mock stream. Confirm the mock reports valid sequential gamepad packets, LunarNX parses the test vibration, video remains stable, and the app log shows Opus frames submitted without steady-state audio drops. Stop both processes after inspection.

- [ ] **Step 4: Inspect bounded app diagnostics and clean generated logs**

Inspect:

```sh
rg -n "audio|rumble|input|rtp|drop|error" "$HOME/work/self/ryujinx-data/sdcard/switch/LunarNX/lunarnx.log"
```

Record only the relevant result in the final response. Remove temporary simulator/app logs after the result is captured. Confirm `git status --short` contains no tokens, logs, NROs, simulator data, or dependency build outputs.

- [ ] **Step 5: Audit repository state**

Run `git status --short` and leave only intentional tracked source/documentation changes. Report the individual implementation commits plus residual real-hardware risk.
