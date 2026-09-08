# Steam Link integration

The Switch build now contains a build-time selectable Steam Link entry point
(`STEAMLINK=1`, the default). The adapter includes:

- Steam host discovery through the Remote Play UDP broadcast protocol.
- Persistent client identity generation under `sdmc:/switch/LunarNX/`.
- Four-digit pairing-code authorization using [ihslib](https://github.com/mariotaku/ihslib).
- Switch UI for host selection, PIN entry, and authorization errors.
- Steam Big Picture streaming requests, H.264 video and Opus audio playback.
- A native generic gamepad: two sticks, triggers, ABXY, D-pad, shoulders,
  stick clicks, View/Menu and Guide; not keyboard emulation.
- Host rumble feedback, neutral input while the UI owns controls, and cleanup
  on disconnect/cancel. One virtual controller is exposed.

Build dependencies are pinned as Git submodules:

```sh
git submodule update --init --recursive
```

The shared `scripts/setup_dependencies.sh` also prepares these root submodules
through `scripts/setup_steamlink_dependencies.sh`, using the revisions recorded
in Git. CI runs `tests/steam_dependency_setup_test.py` against a fresh clone to
verify both headers are fetched and repeated setup retains those revisions.
This test needs network access to the public dependency repositories.

After pairing, enter the optional host security PIN and select Start Steam
stream. The adapter requests Big Picture desktop streaming at up to 1280x720, negotiates
H.264/Opus, and feeds LunarNX's media pipeline. H.264 codec data supplied as
Annex B or AVCDecoderConfigurationRecord is normalized and included with frames.
Pair and start streaming without leaving the Steam host page. Returning to the
platform home recreates the Steam client when reopened, so pair again then too.
Account/device authorization resumption across client instances/app launches is
not implemented yet (only the device identity is persisted).

The pairing direction is important: LunarNX generates and displays the
four-digit pairing code, and the user enters that code in Steam's `Pair Steam
Link` dialog on the host. The host's optional fixed Remote Play security code
is a separate setting, entered in the host security PIN field on LunarNX.

The bundled ihslib discovery, authorization and session sources are linked.
Input is sampled at 125 Hz and sent through ihslib's 48-byte generic HID
protocol. Joy-Con digital triggers map to fully released/pressed; analog source
values retain their range. Basic motor rumble and motion reports are supported;
DualSense effects and multi-player controller slots are not. The ihslib dependency is
LGPL-3.0; keep its license and notices when distributing a build.
The tracked `src/steamlink/ihs_*.c` compile-time overrides keep the pinned submodule
unchanged while serializing HID poll/close and stopping polling before session
channels are destroyed. Both Switch and desktop probe builds use these overrides.

## Motion and touchscreen

Steam is a first-class platform card alongside Xbox and PlayStation. Open the
Steam host page's Settings button to configure touch and gyro using selectors.
These settings are saved separately in `config.json.steam-input`; button mapping
uses `steam_button_mapping` rather than the Xbox profile. The pairing/start page
only handles authorization, the optional security PIN and connection status.
The in-stream Settings entry opens the same Steam-specific page; saved changes
apply on return to the game, and button mappings are reloaded.
The explicit Save and return button persists touch/gyro modes; B discards those
unsaved changes and returns without a filesystem write, even after save failure.
Button mappings have their own save flow.
Steam's Guide action is labeled Steam menu rather than Xbox button. English, Simplified Chinese
and Traditional Chinese are included. Paired hosts can reconnect in the same app
run without starting a second pairing request.

- **Gyro: Native** (default): sends angular velocity and acceleration in the
  generic HID report when Steam requests sensors (command 8). Switch rotations/s
  and g are converted to SDL controller axes, rad/s and m/s². Keep the controller
  stationary for 125 distinct samples (roughly one second) to calibrate bias.
  Handheld, Pro Controller and paired Joy-Con sources are selected automatically.
- **Gyro: Mouse**: hold ZL (default left-trigger mapping) to aim. Does not also
  send native motion, avoiding double movement. Fixed sensitivity is 650 pixels
  per radian, with a small dead zone and a capped timestep after pauses.
- **Gyro: Off**: does not start six-axis sensors.
- **Touch: Trackpad** (default): one-finger movement; short tap for left click;
  hold still for 400ms, then move to drag; lift to release. Two-finger short tap
  gives right click, and two-finger vertical motion scrolls.
- **Touch: Absolute pointer**: places the cursor at the touched screen position
  using the full 1280×720 surface, with the same click/drag gestures. This assumes
  full-screen video; letterboxed/custom aspect ratios require hardware checking.
- **Touch: Off** disables game mouse gestures.

The rightmost 96 pixels are reserved for LunarNX's swipe-in menu: start there
to open it, or start inside the picture to move the game pointer. A held touch
is fenced off until release after the UI takes ownership. Menu/background
ownership neutralizes motion and releases mouse buttons; missing touch/sensor
samples time out instead of leaving a drag or gyro movement running.

`steam-motion` logs sensor initialization, source changes and calibration.
`steam-pointer` logs ownership, valid motion, host sensor requests, angular
velocity and mouse state once per second. `host_sensors=0` in Native mode means
Steam has not requested motion; try Mouse mode if the game's Steam Input
configuration does not expose gyro. Native sensor recognition, coordinate
direction, drift and sensitivity still require real-host/Switch validation.
This does not emulate a PlayStation touchpad.

`python3 tests/steam_pointer_test.py` runs the production gesture/coordinate code
under ASan/UBSan (taps, drag, scroll, UI fencing, menu edge, absolute positioning,
aim gating and missing samples). The HID simulation also verifies sensor
enable/disable requests and saturated gyro/accelerometer report bytes. Neither
test substitutes for physical sensor calibration or a real Steam game session.

## Diagnostics and simulated verification

### Pairing, discovery and audio recovery (2026-09-08)

- Pairing now has a local two-minute deadline even when the host never replies.
  The same page offers **Pair again** after failure, generates a new PIN, and
  keeps the host-side PIN entry instructions visible during the wait.
- `ihs_authorization.c` is a tracked adapter of the pinned client authorization
  implementation. Requests, responses, timer cleanup and cancellation share the
  timer lock; cancellation drains the old task before a retry. Responses must
  match the host ID and source endpoint, and successful responses require a
  nonzero Steam ID. The protocol has no per-attempt authorization request ID;
  a delayed response from the same host across attempts cannot be distinguished
  as reliably as the streaming protocol's request-ID replies.
- A streaming Unauthorized response invalidates only that host's in-memory
  authorization. The failed-start button then offers pairing again. A wrong
  host security PIN does not discard pairing. Multiple hosts retain independent
  authorization within this client instance. Cross-launch authorization
  restoration remains unimplemented.
- Discovery updates existing buttons by host ID instead of clearing the list.
  Updates that delete rows wait until the host page resumes and Borealis has
  restored its focus stack. An expired selected row transfers focus to Refresh.
  Hosts expire after 15 seconds without a status reply; reappearing hosts update
  their endpoint without losing their in-memory authorization.
- The host page accepts a manual IPv4 address and periodically sends unicast
  discovery to port 27036. A real status response is still required before
  pairing; this helps where broadcasts fail but the computer is directly
  reachable. It does not add NAT traversal or Wake-on-LAN. Clearing the address
  stops the unicast retries. Discovery reports troubleshooting guidance rather
  than leaving an unexplained permanent searching state.
- Audio health is independent of video health. After 20 seconds without receive,
  decode or accepted playback submissions, the user receives a nonfatal warning
  with reconnect guidance. An unsupported format also produces a warning.
  Normal host silence does not terminate video. Recovered stages clear the
  warning state; intentional presentation suspension grants a new grace period.
  A successful audio submission is not proof of sound at the physical speaker.

`steam_link_hid_sim_test.py` now also runs 100 authorization cancel/success races,
wrong-host/null response replay, local deadline expiry and immediate retry under
ASan/UBSan. A production-client simulation substitutes only libnx random generation
and exercises host expiry/reappearance/address changes, per-host authorization,
revocation, re-pairing and invalid manual addresses. All files are temporary;
these simulations do not contact a real Steam host. `steam_link_runtime_sim_test.py`
checks independent audio warnings and recovery in addition to its existing video,
input, cancellation and H.264 cases. UI source contracts verify retained rows and
deferred focus restoration, not actual Borealis focus or layout on hardware.

The Docker Switch build uses `devkitpro/devkita64:20251117` with the combined
Xbox/PlayStation/Steam configuration and moonlight curl. The NRO BSS guard passed
at 20.4 MiB. The desktop stream probe also builds with the same authorization
and streaming adapters.

The runtime, HID/client, pointer, settings, release-log and UI/integration checks
passed. The Xbox session-order, DTLS-loop and PPID regressions passed; the existing
SCTP UDP-drain assertion still fails identically in the unchanged main workspace.
No emulator was operated for this follow-up: the available Xbox mock does not
exercise Steam pairing/discovery/media, and emulator interaction remains
user-operated as documented above. Real Steam/Switch tests are still required:
leave a selected host highlighted through several discoveries; cancel/retry
pairing; revoke authorization on the host; try direct-IP discovery; verify
picture, sound and controls, then disconnect and reconnect.


### Independent video liveness, host confirmation and release logs (2026-09-08)

After a session connects, the Steam controller now waits for a successful
renderer presentation before entering Streaming. Decode/renderer enqueue alone
does not satisfy the startup deadline.

During streaming, independent 20-second deadlines cover video receive, decoded
frame output, successful presentation and continuous keyframe recovery.
Audio cannot extend these deadlines. Intentional presentation suspension pauses
the video checks; resuming starts a fresh grace period. Persistent frame counts
are read atomically without taking the media pipeline lock. These checks
diagnose a pipeline that stops making progress while the input/UI loops still
run; they cannot guarantee recovery from an indefinitely blocked driver or UI.

The final host configuration is parsed before NegotiationComplete/Connected.
Explicit Remote HID or input disable is rejected, malformed configuration is
disconnected, and a failed completion send cannot mark the session connected.
An omitted HID flag remains provisional for older hosts rather than being
treated as false. In all cases, the host must actually open our device within
20 seconds. A later device close gets another bounded grace period.
Host rejection is recorded in `SteamNegotiation`; early disconnect and missing
device-open produce user-visible errors instead of an apparently usable stream.

With `APP_DIAG=0`, `steam-health` still records video receive/decode/present counts,
audio count, recovery/suspension state, HID open state and report count every
five seconds and when HID open state changes. Negotiation requests and final
configuration are persistent events, independent of the global Info throttle.
No PIN, credential or individual controller input is added to these summaries.
`tests/steam_release_log_test.py` verifies actual release-mode log output in a
temporary directory, and runs in CI. Final-host protobuf replay covers accept,
reject, absent fields, missing required configuration, input veto and failed
NegotiationComplete send. Runtime simulations cover independent deadlines,
recovery expiry, five minutes of healthy progress, suspension/resume and HID
open/close grace periods.

No emulator was operated for this follow-up. Host HID acceptance, real Steam
recovery timing and Switch rendering/audio still need user-operated validation.
Long deliberate host video pauses may require reconnecting after the 20-second
video deadline; this initial policy favors an actionable failure over an
indefinitely frozen game screen.

### Stream recovery and protocol-error cleanup follow-up (2026-09-07)

- `ihs_discovery.c` is a tracked copy of the pinned discovery channel with
  disconnect initiation latched under the session timer mutex. This also covers
  ihslib's internal error paths. Deinitialization drains the task under the same
  lock, preventing a timer callback from touching an already-freed channel.
  The extended ASan test first failed with a heap-use-after-free at
  `DisconnectTimerEnd`, then passed with the override.
- `ihs_negotiation.c` preserves the pinned negotiation flow but enables Remote
  HID when a provider is registered and the host advertises support. It logs
  that decision and advertises a 1280x720 mode and maximum resolution, matching
  LunarNX's current fixed Steam profile. A host without Remote HID support
  still has no legacy-controller fallback; inspect the host-support log.
  Keep these two copies in sync when upgrading the pinned ihslib revision.
- Decoder recovery is forwarded through `IHS_StreamVideoSubmitReportLost`,
  which ihslib translates into a video-channel StreamDataLost request.
  Requests are limited to once per second, and remain pending until the media
  pipeline actually decodes a fresh IDR. Queue acceptance alone no longer
  suppresses asynchronous recovery. A real host's response is not yet verified.
- Once streaming, 20 seconds without either audio or submitted video becomes
  a visible error; worker finalization also clears the connected state.
  This detects total media silence, not every possible render-only stall.
- Absolute touch uses the fitted video rectangle, including 4:3/16:10 black
  bars, rather than normalizing against the entire Switch screen. Touches
  beginning in a black bar are fenced until release.

The HID simulation now exercises a real protocol-error disconnect before owner
cleanup and captures/parses production negotiation messages for all combinations
of host HID support and registered provider. Portable tests cover recovery
retry timing, media inactivity and letterboxed touch coordinates. These are
component tests, not a full Steam/decoder/Switch end-to-end test.
No emulator interaction or new Ryubing mock-stream run is performed for this
follow-up, per the user's request to operate the emulator themselves.

### Startup/retry and cursor fixes (2026-09-07)

The streaming request has a 20-second application deadline. Cancellation and
timeout now drain the ihslib request/timer callback before allowing a retry;
a duplicate request cannot overwrite the active callback. Tracked adapters
`src/steamlink/ihs_streaming.c` and `ihs_timer.c` serialize streaming state on
the timer lock, including network callbacks, task execution and cleanup. They
wrap the pinned submodule without modifying it. Call request/cancel only from
the owner, outside ihslib callbacks and without holding a base/client mutex.

After the response, session connection has a 15-second deadline, followed by a
20-second first-rendered-frame deadline. Audio packets or undecodable video do
not extend either deadline. Failure logs distinguish the stage and include
video/audio callback counts; the stream exit shows the reason as a notification.
Disconnection callbacks preserve the original timeout error. Session joins and
media teardown remain on the stop worker, not the timer/network/input thread.
Disconnect initiation has its own exactly-once gate, independent of the Error
state. The later discovery-channel fix above also covers library-internal
disconnect calls, which this controller-only gate could not prevent.
The POSIX session socket uses a 10ms receive timeout so join can finish
even when the host sends no more packets. The desktop thread adapter matches
SDL's recursive mutex contract used on Switch.

Remote cursor show/hide/select/delete/image events now feed a UI-thread overlay,
hidden while LunarNX owns input. Raw RGBA images require an exact byte count and
a valid hotspot; dimensions are limited to 256x256 with eight cached images.
Unknown/rejected images use a fallback arrow only while the host requests a
visible cursor. Rendering accounts for video letterboxing and capture scaling.
Relative touch/gyro motion predicts the cursor locally; host acceleration can
cause drift until a host ShowCursor update. Verify accuracy in an actual game.
The RGBA format is supported by the protocol research in
[Thalium's Remote Play analysis](https://blog.thalium.re/posts/achieving-remote-code-execution-in-steam-remote-play/).

The cursor is explicitly detached from the column layout before being added
over the video. Source contracts verify its full-screen overlay geometry; the
actual Borealis/NanoVG output still needs user-operated testing.

Desktop ASan/UBSan checks include 100 real ihslib timer/request/cancel/retry
cycles with a loopback UDP receiver, stale request IDs and callbacks racing
cancellation, plus 20 real session disconnect/join/destroy cycles covering no
host response and a host disconnect racing cleanup. Portable tests cover startup deadline boundaries and cursor
validation, cache bounds, immutable snapshots, coordinates and reset. The tests
run in CI alongside input simulations. These are component tests, not a native
Borealis cursor rendering test, Steam authentication test, or successful
end-to-end Switch stream. No emulator was operated for this fix at the user's
request. Hardware checks still needed: blocked host port, connected-without-
video, immediate retry, actual visible cursor/hotspot, gamepad, sound and video.

Build Switch artifacts only in Docker with STEAMLINK=1 APP_DIAG=1 DROP_DIAG=1.
Read `sdmc:/switch/LunarNX/lunarnx.log` after a failed test. Phase tags include
steam-link, steam-session, steam-video, steam-audio, steam-input and steam-hid. Protocol
Debug/Verbose output is discarded; Warning/Info forwarding is limited to one
message per level per second. Error messages and explicit phase logs remain.

Run `python3 tests/steam_link_runtime_sim_test.py` on a desktop with clang++ and
FFmpeg/libx264. It exercises the production cancellation, input pump, key edge
retry, session cleanup and codec-data helpers under ASan/UBSan. A generated
H.264 stream has SPS/PPS separated from its frames, is reconstructed using the
production helper, and decoded to matching frame hashes. This does not emulate
Steam authentication, libnx input, NVDEC, audio output or a real Steam session.
The Xbox Ryubing mock cannot validate the Steam wire protocol. On 2026-09-07,
the Docker-built NRO loaded in Ryubing Canary 1.3.333, completed root activity
creation and entered the main loop. This is only a startup smoke test; no
Ryubing mock-stream or real Switch Steam session was run for these changes.
The NRO BSS guard passed at 20.4 MiB. The separate legacy WebRTC SCTP static
check fails on its UDP-drain assertion in both this worktree and the unchanged
main checkout; the other requested WebRTC checks passed.

Run `python3 tests/steam_link_hid_sim_test.py` with clang and the desktop mbedTLS
libraries used by the desktop probe (`build_host/library`). It compiles the
production provider and ihslib with ASan/UBSan, checks all button and axis
mappings, serializes/parses real HID protobuf reports, and exercises host-like
feature, rumble, full-resync and close requests. It also runs 100 concurrent
timer polling/close/reopen cycles. This is a protocol-component simulation,
not a successful connection to Steam or a hardware rumble test.

### First Switch test

1. Keep Steam and Switch on the same LAN and enable host Remote Play.
2. Open LunarNX's Steam entry, select the host, and enter the displayed pairing
   code in Steam. Enter the separate host security PIN in LunarNX if configured.
3. Start streaming in this same app run. In Big Picture, select a game with
   controller support. Check both sticks, ABXY, triggers and the in-stream menu.
4. Return through the LunarNX overlay and reconnect to check input cleanup.

If it fails, retain the app log. `gamepad device list sent` means enumeration
was queued, not that Steam accepted it. `host_opened=1` means Steam opened the
virtual controller; increasing `reports` counts locally generated reports,
not host acknowledgements. `buttons`, `lx/ly/rx/ry`, `lt/rt` and
`rumble_commands` distinguish input capture from host device/feedback issues.
Non-zero video/audio callbacks and visible/audible output are separately
required; a successful pairing alone is not an end-to-end streaming pass.

## Desktop discovery probe

### UI verification (2026-09-07)

The three equal platform cards and entry into Steam discovery/settings were
observed in Ryubing Canary 1.3.333. This exposed two issues: repeated discovery
startup from Refresh and saving over an existing settings file. Refresh now
reuses the periodic discovery task. Settings save rotates the old file to
`.bak` before committing `.tmp`, avoiding overwrite-by-rename assumptions on
Switch. Failed commits restore the old file; loading falls back to `.bak` if
the main file is missing. This is recoverable replacement, not a guarantee of
atomic persistence during power loss. Logs use `[steam-settings]` with the save
stage and errno, or the saved mode values (no credentials).

`python3 tests/steam_settings_file_test.py` uses real temporary files and an
injected no-overwrite rename implementation under ASan/UBSan. It covers first
and repeated saves, commit/restore failures and backup recovery. It is not a
Switch filesystem or UI test. `tests/steam_ui_contract_test.py` checks static
wiring and locale keys only. Pointer/HID/runtime simulation tests also passed.

Further emulator interaction is user-operated. The user confirmed saving works
after restarting the fixed NRO. Reopen the new NRO, change a
mode, save, reopen settings, then change and save again to verify persistence.
One earlier emulator run exited in macOS CAMetalLayer/objc_release; this is
recorded separately from guest behavior. No actual Steam media/input round trip
or Switch hardware compatibility is certified by these checks. The existing
`libpeer_sctp_config_test.py` drain-loop assertion also fails on main and is not
changed by this Steam UI work.

The dependency-free probe can validate the network discovery path without a
Switch NRO:

```sh
python3 tools/steamlink_probe.py --target 255.255.255.255 --timeout 6
```

Use `--target <host-ip>` to avoid broadcast while testing a known host. The
probe parses the same discovery framing and status fields used by LunarNX.

For authorization, `tools/steamlink_probe/authorize.c` is a small POSIX
ihslib client. It must be compiled with the vendored ihslib sources and a
desktop mbedTLS build, then run with the four-digit pairing code as its only
argument. Authorization should be performed only against a Steam host you
control; the code is passed on the local command line and is not stored.

## Desktop stream probe

The desktop probe links the same ihslib session sources used by the eventual
Switch adapter. It requests the host desktop, negotiates H.264 plus Opus, saves
the received elementary streams, and opens H.264/HEVC video in `ffplay` when it
is installed:

```sh
tools/steamlink_probe/build_desktop_stream_probe.sh
build/steamlink-desktop-stream-probe --pin <remote-play-security-pin> --duration 60
```

`--pin` is the host's Remote Play security PIN and is separate from the
four-digit pairing code. If the device was paired by an earlier process, use
`--pair-code <four-digit-pairing-code>` to authorize and stream in one process;
this is useful for testing clients that do not yet persist Steam's device token:

```sh
build/steamlink-desktop-stream-probe --pair-code <pairing-code> \
  --pin <remote-play-security-pin> --duration 60
```

`--no-display` keeps the probe headless while still recording and counting media
callbacks. A successful end-to-end run must show
`streaming request success`, `session connected`, a negotiated `video start`,
and non-zero video/audio packet summaries. A host with no running game may
reject the request or require a security PIN; that is a host-state result, not
evidence that the session transport works.

### Teardown, media metadata and foreground recovery audit

The tracked adapters now detach a removed channel from the session table before
joining/destroying its worker, decrement the table count for every position and
clear the vacant slot. Duplicate/capacity-rejected channels are stopped and
released. Table mutations and disconnect use the session timer mutex; worker
joins occur outside it. Disconnect acquires timer before base, matching timer
callbacks. A receive failure interrupts all workers before joining the sender.
Overlapping `SteamLinkClient` instances share a synchronized process runtime
lease, keeping the global timer service alive until the last client is gone.

The controller serializes start/stop separately from the mutex used by drawing
and settings. The host-response wait releases that UI-facing mutex. Drawing
skips busy lifecycle work, suspension is submitted atomically, and background /
foreground transitions reset progress deadlines. Failed partial startup is
cleaned up on the network worker before posting its result to Borealis.

Unsupported audio is discarded with a warning while video and input continue.
Audio channel restart resets audio queues, reorder, decoder and playback state
without resetting video. The audio callback receives the real 16-bit frame ID,
including gaps and rollover. Audio and assembled video retain the host's
1/65536-second timestamps; a synchronized shared clock unwraps them into a
common nanosecond timeline. Video uses the first assembled fragment's timestamp.
Metadata accessors are valid only synchronously inside their respective callback
and use thread-local storage, avoiding a process-global active-session pointer.

Malformed broadcast protobuf payloads and status messages without a hostname
are discarded before dispatch. Data-worker startup failure releases its temporary
frame buffer. A timed-out GPU context remains quarantined because the GPU may
still reference its resources; once quarantined, renderer initialization is
blocked for the process and Steam startup asks the user to restart LunarNX.
This bounds repeated quarantine accumulation rather than freeing unsafe memory.

The ASan/UBSan production-protocol replay now covers middle-channel deletion,
base/timer lock order, malformed status datagrams, audio ID gaps/rollover, video
fragment timestamp retention, injected receive failure and overlapping client
lifetimes. Portable tests cover host-clock rollover/reordered A/V timestamps,
reconstructed H.264 output, audio reorder/timing and A/V sync. The Switch Docker
build and BSS guard, desktop probe build, foreground/media/renderer contracts and
whitespace check are required for these changes. The pre-existing libpeer SCTP
UDP-drain assertion remains a separate failing check.

No new emulator smoke test was performed: interaction remains user-operated,
and the Xbox mock cannot exercise this Steam transport. These local replays do
not verify physical audio output, actual host timestamp behavior, real Switch
GPU fault recovery or prolonged Steam streaming; those remain hardware checks.

### UI recovery and navigation follow-up

Steam streams now open the LunarNX menu with Minus + Plus, including in docked
mode. The chord is reserved from game input; B closes the menu and its Disconnect
action keeps confirmation. Xbox/PlayStation retain their existing exit shortcut.
A normal short tap in the right-edge gesture area is deferred until release,
then delivered as a mouse click (with its position in Absolute mode). A leftward
menu swipe never produces a host click. Edge drags remain reserved for gestures.

Repeated foreground events coalesce recovery without retaining stale background
state. Completion respects current focus and an open settings page before
resuming presentation/input. The playback page shows a waiting-for-video message
until streaming starts, and hides that message while its menu is open. The host
list is scrollable independently of its toolbar.

A successful authorization refreshes the host before enabling Start. Authorized
endpoints remain available for a direct streaming attempt after discovery expiry;
new discovery replies update them and Unauthorized clears the cached endpoint.
This cache belongs to the current client instance and is not persisted.

Button-mapping edits and Reset only update the displayed mapping after a
successful save. Saves use a flushed temporary file and recoverable replacement;
write/close failures preserve the previous config and show an error. Touch/gyro
settings still have an explicit Save action. The discard label and wrapping help
now explicitly distinguish those unsaved changes from independently auto-saved
button mappings; discard does not roll back mappings.

`tests/steam_ux_replay_test.py` executes the production foreground and mapping
handlers with controlled worker scheduling and injected save failure under
ASan/UBSan. Pointer and production-client tests cover edge taps versus swipes and
pairing success/stream requests after discovery expiry. Settings-file tests cover
flush failure preservation. These tests are not a rendered Borealis UI run.
No new Ryubing smoke run was performed: emulator interaction remains user-operated
and the available Xbox mock does not validate Steam. Docked interaction, visual
layout with many hosts and real Steam picture/audio still require device testing.

### Menu release, recording cancellation and disconnect feedback

Steam waits for released buttons/triggers and centered sticks after closing its
menu, leaving settings or recovering foreground input. Small stick drift is
accepted. Repeated resume calls cannot remove this fence, and a neutral sample
from an older UI visit cannot unlock a newer visit. Explicit Steam menu commands
still send their virtual Home pulse. Xbox/PlayStation keep their existing routing
contract because they inject their virtual buttons before routing.

During mapping capture, press Minus + Plus together to cancel without changing
or saving the mapping. B remains assignable. The capture instructions explain the
shortcut in all three locales. An unexpected session disconnect now displays a
localized notice directing the user to check the PC/network, select the host and
Start streaming again; authorization is retained within the current client.

Validation: final combined Docker Switch build and BSS guard passed (20.4 MiB).
ASan/UBSan input routing tests cover held buttons/triggers/sticks, repeated UI
transitions, drift and unchanged legacy routing. Production handler replay covers
cancellation through the polling handler, unchanged saved mapping and assigning B.
Steam HID/client and runtime replays, pointer/settings regressions, UI contracts,
localization structure, PS/Xbox virtual-button checks, Xbox session ordering,
DTLS-loop and PPID checks passed. The existing SCTP UDP-drain assertion still
fails. No new Ryubing run: emulator interaction remains user-operated and the
Xbox mock does not exercise Steam. This does not replace real-host/Switch testing.

### Menu chord and pointer follow-up

Steam buffers an initial Minus/Plus press for 120 ms to recognize the menu chord
before forwarding either key. A short standalone tap is preserved as a 40 ms
pulse; a standalone hold is forwarded after the recognition window. Once the
chord is seen, both keys remain suppressed until both are released. Xbox and
PlayStation retain their existing behavior. Press the two keys together: a
second press after the recognition window cannot undo an already forwarded
standalone press.

Absolute-mode two-finger taps position the pointer at the two-finger center
before the right-button event, including staggered finger release and letterboxed
video coordinates. Edge gesture detection continues gyro mouse updates while
waiting for a tap/swipe, even when touch mouse input is Off. UI ownership still
suppresses all physical pointer and motion output.

ASan/UBSan input/gesture regressions cover both menu-key orders, staggered chord
release, standalone taps/holds, absolute right-click position, letterboxing,
staggered finger lift, and edge-touch gyro behavior in every touch mode. The
production UI replay and Steam UI/integration plus PS/Xbox button regressions
also pass. No new Ryubing smoke run was performed: emulator interaction remains
user-operated, and the available Xbox mock cannot validate the Steam transport.
Real Switch/Steam control feel and picture/audio remain device validation items.
