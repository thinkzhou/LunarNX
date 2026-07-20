# Audio And Input Reliability Design

## Goal

Make LunarNX's Xbox controller, rumble, Opus receive, audio playback, and A/V
clock paths correct enough for real Xbox and real Switch validation while
preserving the existing Moonlight-aligned `libopus + audren` output path.

## Scope

This change will:

- align Xbox input packet bytes with XStreaming;
- resend controller state at the Xbox polling cadence and reset state on every
  session or reconnect;
- pass RTP sequence numbers and timestamps out of legacy libpeer;
- process audio on a dedicated worker;
- reorder a small number of out-of-order Opus packets and invoke Opus packet
  loss concealment for missing packets;
- derive stable media timestamps from RTP clocks instead of callback arrival
  jitter;
- use the estimated audren playback position as the audio master clock;
- make controller and rumble initialization repeatable;
- teach the mock Xbox server to validate input packets and optionally send a
  deterministic vibration report.

This change will not add multiple controllers, touchscreen input, mouse,
keyboard, motion sensors, configurable mappings, surround audio, or Bluetooth
audio-specific handling.

## Reference Boundaries

- Switch audio output follows Moonlight-Switch's `AudrenAudioRenderer`: libopus
  multistream decode, signed 16-bit PCM, libnx audren, five wave buffers, and
  explicit cache flushes.
- Xbox input and vibration packet bytes follow XStreaming's
  `src/webrtc/Packet/index.ts` and `src/webrtc/Channel/Input.ts`.
- Legacy libpeer remains the active WebRTC provider. Any local libpeer source
  change must also be reproducible through
  `tools/libpeer_legacy/legacy-libpeer-switch.patch`.

## Input Protocol

### Packet Encoding

`XInputEncoder` keeps the existing 14-byte header and 23-byte gamepad frame.
The button mask remains unchanged. `PhysicalPhysicality` uses the Xbox
physicality values, which are distinct from button-mask values:

```text
DPadUp=0x00000001       Menu=0x00000010
View=0x00000020         LeftThumb=0x00000040
RightThumb=0x00000080   LeftShoulder=0x00000100
RightShoulder=0x00000200 Nexus=0x00000400
A=0x00001000            B=0x00002000
X=0x00004000            Y=0x00008000
LeftTrigger=0x00010000  RightTrigger=0x00020000
LeftThumbX=0x00040000   LeftThumbY=0x00080000
RightThumbX=0x00100000  RightThumbY=0x00200000
```

Only active controls are included, matching XStreaming. A non-idle stick sets
both physicality bits for that stick. `VirtualPhysicality` remains zero.

libnx stick Y is already positive when pushed upward. The encoder writes that
value directly to the Xbox wire rather than negating it. The timestamp field
uses milliseconds from a process-local steady clock, matching the monotonic
semantics of `performance.now()`.

### Polling And Lifecycle

The stream loop sends one current gamepad packet every 16ms while control and
transport are ready, even when the state is unchanged. This matches
XStreaming's default 62.5Hz polling behavior and guarantees state recovery
after channel disruption.

`XInputEncoder::reset()` clears sequence and cached state. It is called at the
start of a session and after WebRTC reconnect before metadata is sent.
`GamepadReader::initialize()` deletes any previously allocated `PadState`,
clears the stored pointer, then allocates and initializes a new one.

## RTP Metadata Contract

Legacy libpeer's decoded RTP callback will include:

```c
typedef void (*RtpOnPacket)(uint8_t* data,
                            size_t size,
                            uint16_t sequence,
                            uint32_t timestamp,
                            void* user_data);
```

Generic Opus forwarding passes the packet's RTP sequence and timestamp.
H.264 forwarding passes the assembled frame's RTP timestamp and the last RTP
sequence contributing to that frame. Existing RTP statistics remain intact.

`PeerManager` maps RTP timestamps onto a common monotonic nanosecond timeline:

- video clock rate: 90000Hz;
- audio clock rate: 48000Hz;
- first packet arrival establishes each track's anchor;
- subsequent timestamps use wrap-safe RTP deltas from that anchor;
- callback arrival time is not used after anchoring.

This is not full RTCP sender-report synchronization, but it removes per-packet
arrival jitter while preserving the initial audio/video arrival offset.

## Audio Queue And PLC

Video keeps the existing media worker. Audio receives a dedicated queue,
condition variable, and worker so `audrenWaitFrame()` cannot block video
decode.

The audio queue stores payload, RTP sequence, mapped timestamp, and generation.
It is bounded to 512 packets and 4 MiB. On overflow it discards the oldest
audio packet, records an audio drop, and advances without blocking the WebRTC
receive loop.

The worker maintains a four-packet reorder window:

1. In-order packets decode immediately.
2. Slightly early packets are retained until the missing sequence arrives or
   the window is exceeded.
3. When a gap becomes unrecoverable, one PLC frame is generated for each
   missing packet, capped at three consecutive PLC frames.
4. Late packets older than the expected sequence are discarded.
5. A forward discontinuity larger than 64 packets resets the reorder state
   instead of synthesizing a long period of audio.

`AudioDecoder` obtains normal packet duration through
`opus_packet_get_nb_samples()`. It remembers the latest valid duration and
uses that duration for `decodeMissing()`, which calls
`opus_multistream_decode()` with a null payload. The default before the first
valid packet is 960 samples at 48kHz.

## Audio Playback Clock

`AudioPlayer::play()` continues to submit PCM through audren. It exposes the
current queued sample count under its existing mutex.

After a frame is accepted, the media pipeline computes the estimated playback
position:

```text
frame end timestamp - queued audren duration
```

That position updates `AVSync` and becomes the audio master clock used to
schedule video. Failed or dropped audio writes do not advance the audio clock.
If audio stops updating for 500ms, the existing wall-clock fallback remains
active.

## Rumble

`RumbleController::initialize()` stops any active vibration, clears stale
handles, and probes handheld then player-one controller handles each time it is
called. Existing four-motor mapping remains:

- Xbox left/right motors -> Switch low-frequency channels;
- Xbox trigger motors -> Switch high-frequency channels;
- strength remains scaled to 50%.

The input-channel parser follows XStreaming's section order: optional
`ServerMetadata` first, then the 11-byte vibration payload. Unsupported mixed
sections are rejected with a bounded diagnostic instead of guessed offsets.

## Mock And Tests

The mock Xbox server will parse every gamepad packet and validate:

- exact packet length and frame count;
- sequence progression;
- button mask;
- signed stick axes and trigger values;
- calculated physical and virtual physicality.

A `--send-test-rumble` option sends one known vibration packet after the first
valid gamepad packet. It is disabled by default.

Automated coverage will include:

- exact neutral, A-button, trigger, and stick packet bytes;
- positive-up Y axes;
- encoder reset and repeated-state transmission;
- RTP sequence/timestamp propagation through legacy libpeer;
- RTP clock wrap handling;
- in-order audio, out-of-order audio, one-packet PLC, late packet discard, and
  large discontinuity reset;
- audio/video worker independence;
- playback-clock subtraction of queued audren samples;
- vibration packet parsing and mock validation.

## Error Handling

- Input send failure is logged and retried on the next 16ms poll.
- Invalid input-channel vibration payloads are ignored without affecting
  media.
- Audio queue allocation or overflow records a drop and keeps streaming.
- Opus decode or PLC failure records an audio drop and does not advance A/V
  sync.
- Worker shutdown remains generation-based and joins both workers before
  decoder or audren resources are destroyed.

## Verification

Completion requires:

1. All focused input, data-channel, audio, RTP, and session tests pass.
2. Desktop stream tests pass.
3. Docker `devkitpro/devkita64:20251117` clean Switch build passes.
4. Ryubing mock stream shows Opus frames submitted with no steady-state audio
   drops, video remains stable, input validation passes, and optional rumble is
   received.
5. Generated logs, simulator data, tokens, and NRO files remain uncommitted.

Real Switch hardware remains the final validation target for audible output,
controller direction, rumble device selection, long-run A/V sync, and packet
loss behavior.
