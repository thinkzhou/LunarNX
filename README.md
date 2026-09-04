# LunarNX

English | [简体中文](README.zh-CN.md)

![LunarNX — Xbox, PlayStation, and cloud gaming on Nintendo Switch](res/appstore/banner.png)

An unofficial Xbox and PlayStation game-streaming client for Nintendo Switch homebrew.

LunarNX brings Xbox Remote Play, Xbox Cloud Gaming, and PlayStation Remote Play into one controller-first Switch application.

**Download the latest build from [GitHub Releases](https://github.com/thinkzhou/LunarNX/releases/latest).**

## Highlights

- Xbox Remote Play from your own Xbox One or Xbox Series console
- Xbox Cloud Gaming library, recent titles, search, and playback
- PlayStation Remote Play for paired PS4 and PS5 consoles
- Local PlayStation discovery, pairing, wake-up, and streaming
- PlayStation Network sign-in, PS5 device discovery, and remote streaming through PSN
- 720p, 1080p, and 1080p HQ quality profiles shared by Xbox and PlayStation
- Hardware-accelerated H.264 and PS5 HEVC video, low-latency audio, and responsive controller input
- Separate Xbox and PlayStation button-mapping profiles, including multi-button combinations
- PlayStation touchpad gestures and Switch six-axis motion forwarding
- PlayStation rumble and DualSense haptics forwarded to Switch vibration
- In-stream performance monitoring, platform Home/Guide button, and safe disconnect controls
- Stream recovery after returning from the HOME menu
- Optional image upscaling and sharpening
- English, Simplified Chinese, and Traditional Chinese interfaces

Xbox Remote Play on local and remote networks, Xbox Cloud Gaming, local PS4 and PS5 Remote Play, and PS5 Remote Play through PSN have all been tested on real Nintendo Switch hardware. These paths can stream video and audio and accept controller input; availability still varies with console configuration, account permissions, NAT behavior, and network quality.

> [!WARNING]
> LunarNX is early-stage homebrew software. It requires a modified Nintendo Switch capable of running NRO applications and does not work on an unmodified retail console.

## Compatibility

| Feature | Status |
| --- | --- |
| Xbox Remote Play on the same LAN | Tested on real Switch hardware |
| Xbox Cloud Gaming | Tested on real Switch hardware |
| PS5 Remote Play through PlayStation Network | Tested on real Switch hardware |
| Local PS5 discovery, pairing, wake-up, and streaming | Tested on real Switch hardware |
| Local PS4 discovery, pairing, wake-up, and streaming | Tested on real Switch hardware |
| 720p / 1080p / 1080p HQ | Available for Xbox and PlayStation sessions |
| Xbox Remote Play over the internet | Tested on real Switch hardware; requires a compatible direct network path |

Remote streaming still depends on account permissions, console settings, NAT behavior, Sony/Microsoft services, and network quality. A successful test on one network does not guarantee connectivity on every network.

## Requirements

- A Nintendo Switch capable of running homebrew NRO applications, typically with Atmosphere CFW
- Homebrew Menu running through title override/full-memory mode
- A stable 5 GHz Wi-Fi or wired network connection

For Xbox:

- An Xbox One or Xbox Series console with Remote features enabled, or
- A Microsoft account eligible for Xbox Cloud Gaming

For PlayStation:

- A PS4 or PS5 with Remote Play enabled
- A pairing PIN from the console for first-time local registration
- A PlayStation Network account for remote PS5 discovery and streaming through PSN

Xbox Cloud Gaming availability depends on account entitlement and region. PlayStation remote connectivity depends on the console, PSN account, NAT, and network environment.

## Installation

1. Download the latest ZIP or NRO asset from the [Releases page](https://github.com/thinkzhou/LunarNX/releases). Release tags identify versions; current assets use stable filenames.
2. Install it at the following path:

   ```text
   sdmc:/switch/LunarNX/LunarNX.nro
   ```

   The ZIP already contains `LunarNX/LunarNX.nro`; extract that directory at the SD card root. For a standalone NRO, copy it to the path above.

3. Open Homebrew Menu using title override/full-memory mode, then launch LunarNX.

Do not share the contents of `sdmc:/switch/LunarNX/`. It may contain Microsoft tokens, PlayStation credentials, PSN session data, console identifiers, and diagnostic logs.

## Getting Started

LunarNX opens with a platform selector. Choose Xbox or PlayStation.

### Xbox

1. Start Microsoft sign-in.
2. Scan the QR code with a phone on the same network, or open the displayed device-login URL and enter the code.
3. Select a registered Xbox console or open the Xbox Cloud Gaming library.
4. Choose a quality profile in Settings.
5. Select **Play**, **Connect**, or **Wake & connect**.

### PlayStation

For local play:

1. Enable Remote Play on the PS4 or PS5.
2. Search the local network, or select **Pair by IP**.
3. On the console, open the Remote Play device-registration screen and enter its pairing PIN in LunarNX.
4. Select the paired console and connect. A console in rest mode can be woken when the required registration data is available.

For remote PS5 play through PSN:

1. Open the PlayStation page and choose PlayStation Network sign-in.
2. Scan the displayed QR code with a phone on the same network, complete Sony sign-in, and send the result back to LunarNX from the phone helper page.
3. Refresh the PSN device list.
4. Select a PS5 with Remote Play enabled.
5. Wait while LunarNX creates the PSN session, establishes the control and data paths, and starts the Chiaki streaming session.

If the PlayStation user profile has a four-digit login PIN, LunarNX will request it during connection.

Start with 720p if network quality is uncertain. PSN connection establishment can take up to a minute on some networks.

## Controls

LunarNX maps face buttons by physical position so the bottom, right, left, and top buttons match the remote console layout.

| Nintendo Switch | Xbox | PlayStation |
| --- | --- | --- |
| B (bottom) | A | Cross |
| A (right) | B | Circle |
| Y (left) | X | Square |
| X (top) | Y | Triangle |
| L / R | LB / RB | L1 / R1 |
| ZL / ZR | LT / RT | L2 / R2 |
| Minus | View | Share |
| Plus | Menu | Options |
| L + R + Plus | Xbox Guide / Nexus | PS button |
| Left / right stick click | L3 / R3 | L3 / R3 |
| D-pad / sticks | D-pad / sticks | D-pad / sticks |

ZL and ZR are digital Switch buttons, so analog trigger pressure is reported as either 0% or 100%.

Xbox and PlayStation button mappings can be customized independently in each platform's settings. Single buttons and button combinations are supported; LunarNX marks conflicting mappings and can restore the defaults.

The Switch Capture button can also be assigned. For example, assign **Capture** to **Guide** in the Xbox mapping to use it as the Xbox/Nexus button while streaming. LunarNX temporarily takes control of the Capture button only when a mapping uses it, then restores the system screenshot behavior after the stream ends.

- Swipe left from the right edge of the touch screen to open the stream menu.
- The menu shows performance information and uses the correct **Xbox button** or **PS button** label for the active platform.
- Press **L + R + Plus** to send the platform Guide/PS button.
- To stop streaming with buttons, press **Minus + Plus together twice within three seconds**.
- The touch menu also requires a second confirmation before disconnecting.
- During a PlayStation stream, touches and swipes outside the menu gesture area are forwarded to the DualShock/DualSense touchpad. A short tap or long press sends a touchpad click.
- During a PlayStation stream, supported Joy-Con, Pro Controller, and handheld-mode motion data is forwarded as controller gyroscope, accelerometer, and orientation input.
- During supported PlayStation sessions, remote rumble and DualSense haptics are converted to Switch vibration.

## PlayStation Notes

- PlayStation protocol code is isolated under `src/ps/` and uses [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng).
- Remote PS5 streaming uses PlayStation Network signaling and Chiaki hole punching. Restrictive NAT, firewall rules, Wi-Fi loss, or PSN service behavior can still prevent a connection.
- PS5 sessions can use H.264 or HEVC; PS4 sessions use H.264. Requested codec, resolution, and bitrate come from the LunarNX quality settings.
- Exiting a stream stops and joins the Chiaki session, finalizes the session, releases PSN/hole-punch resources, and shuts down the media pipeline.
- PSN credentials and registration files are private. Never include them in an issue or log upload.

## Troubleshooting

- **The app does not start:** launch Homebrew Menu with title override. Applet Mode may not provide enough memory for streaming and hardware decoding.
- **Video stutters or becomes blocky:** use 5 GHz Wi-Fi, move closer to the access point, reduce competing traffic, or try 720p.
- **Xbox is not found:** confirm Remote features are enabled and the Xbox and Switch are on the same LAN.
- **Xbox internet Remote Play cannot connect:** the connection currently needs a direct path; restrictive NAT or firewalls may block it.
- **Cloud games are unavailable:** confirm that the Microsoft account and region are eligible for Xbox Cloud Gaming.
- **PlayStation is not found locally:** confirm Remote Play is enabled, both devices are on the same LAN, and client isolation is disabled on the access point.
- **PlayStation pairing fails:** verify the console IP, console type, and current eight-digit Remote Play pairing PIN.
- **PSN device list is empty:** confirm the PSN account is the account linked to the PS5 and that Remote Play is enabled on the console.
- **Remote PS5 connection fails:** retry on a stable network. NAT behavior, PSN signaling, UDP hole punching, and console state all affect the result.
- **The console asks for a login PIN:** enter the four-digit PIN configured for that PlayStation user profile.

## Screenshots

The existing screenshots show the Xbox interface; updated PlayStation screenshots will be added as the UI stabilizes.

| Console discovery | Stream settings |
| --- | --- |
| ![LunarNX Xbox console discovery screen](docs/screenshots/find_xbox.jpg) | ![LunarNX resolution and decoder settings](docs/screenshots/settings.jpg) |

![LunarNX establishing a streaming session](docs/screenshots/connecting.jpg)

## Community, Author Support, And Privacy

### Community

- Project author: [thinkzhou](https://github.com/thinkzhou)
- LunarNX QQ group: **736743823**
- LunarNX Discord: [discord.gg/cFZj8mpg2K](https://discord.gg/cFZj8mpg2K)
- Issues and releases: [GitHub repository](https://github.com/thinkzhou/LunarNX)

### Support The Author

LunarNX is free and open-source software. If it is useful to you and you would like to support continued development, you can use either option below.

| WeChat Pay | Alipay |
| --- | --- |
| <img src="romfs/img/support/wechat.png" alt="WeChat Pay support QR code" width="220"> | <img src="romfs/img/support/alipay.png" alt="Alipay support QR code" width="220"> |

Support is entirely optional and does not affect access to releases, features, or issue reporting.

### Issue Reports And Privacy

Before reporting a problem, read [SECURITY.md](SECURITY.md). Never post token files, PS credential files, complete logs, raw service responses, console identifiers, account IDs, or public IP addresses. A useful report includes the selected platform and quality profile, Switch model and firmware, local/cloud/PSN connection type, network type, and only the smallest redacted log excerpt needed.

## Development

Build instructions, validation commands, architecture, and testing notes are documented in the [development guide](docs/development.md). Contributions are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).

Switch builds must use Docker/devkitA64. The combined Xbox/PlayStation build uses the bundled curl 8.x WebSocket build and the legacy local libpeer path:

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    set -e
    export DEVKITPRO=/opt/devkitpro
    export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
    make -f Makefile.switch -j$(nproc) \
      NETWORK_DIAG=0 CURL_PROVIDER=moonlight CURL_VERIFY=0 \
      CURL_VERBOSE=0 CURL_TIMEOUT_MS=30000
  '
```

Real Nintendo Switch hardware remains the final compatibility target.

## Credits And License

LunarNX builds on [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng), [libpeer](https://github.com/sepfy/libpeer), [Borealis](https://github.com/XITRIX/borealis), [FFmpeg](https://github.com/FFmpeg/FFmpeg), [libnx](https://github.com/switchbrew/libnx), [deko3d](https://github.com/devkitPro/deko3d), [Moonlight-Switch](https://github.com/XITRIX/Moonlight-Switch), and other projects listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Xbox and PlayStation protocol behavior and client workflows were also studied with [XStreaming](https://github.com/Geocld/XStreaming) and [PeaSyo](https://github.com/Geocld/PeaSyo); they are reference projects rather than bundled LunarNX runtime dependencies.

Original LunarNX code outside the PlayStation module is available under the MIT License. PlayStation support links against chiaki-ng under AGPL-3.0. A combined binary containing the PlayStation path must be distributed under AGPL-3.0 terms with the complete corresponding source code and applicable dependency modifications available. See [LICENSE](LICENSE), [LICENSES/AGPL-3.0.txt](LICENSES/AGPL-3.0.txt), and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

LunarNX is not affiliated with, authorized by, or endorsed by Microsoft, Xbox, Sony Interactive Entertainment, PlayStation, Nintendo, or the maintainers of the referenced projects. All product names and trademarks belong to their respective owners.
