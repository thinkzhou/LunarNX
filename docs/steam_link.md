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
The small `src/steamlink/ihs_*.c` compile-time overrides keep the pinned submodule
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
state, so watchdog/exit/host-disconnect cannot schedule duplicate disconnect
timers. The POSIX session socket uses a 10ms receive timeout so join can finish
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
