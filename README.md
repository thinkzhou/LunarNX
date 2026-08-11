# LunarNX

English | [简体中文](README.zh-CN.md)

An unofficial Xbox and PlayStation game-streaming client for Nintendo Switch homebrew.

LunarNX brings Xbox Remote Play, Xbox Cloud Gaming, and PlayStation Remote Play into one controller-first Switch application.

![LunarNX streaming a game at 1080p on Nintendo Switch](docs/screenshots/streaming.jpg)

## Highlights

- Xbox Remote Play from your own Xbox One or Xbox Series console
- Xbox Cloud Gaming library, recent titles, search, and playback
- PlayStation Remote Play for paired PS4 and PS5 consoles
- Local PlayStation discovery, pairing, wake-up, and streaming
- PlayStation Network sign-in, PS5 device discovery, and remote streaming through PSN
- 720p, 1080p, and 1080p HQ quality profiles shared by Xbox and PlayStation
- Hardware-accelerated H.264 video, low-latency audio, and responsive controller input
- In-stream performance monitoring, platform Home/Guide button, and safe disconnect controls
- Optional image upscaling and sharpening
- English, Simplified Chinese, and Traditional Chinese interfaces

Xbox Remote Play, Xbox Cloud Gaming, and PS5 Remote Play through PSN have been tested on real Nintendo Switch hardware. The PlayStation path can stream continuously with video, audio, and controller input. Local PS4/PS5 support is implemented but still needs broader testing across console models, firmware versions, and network environments.

> [!WARNING]
> LunarNX is early-stage homebrew software. It requires a modified Nintendo Switch capable of running NRO applications and does not work on an unmodified retail console.

## Compatibility

| Feature | Status |
| --- | --- |
| Xbox Remote Play on the same LAN | Tested on real Switch hardware |
| Xbox Cloud Gaming | Tested on real Switch hardware |
| PS5 Remote Play through PlayStation Network | Tested on real Switch hardware |
| Local PS5 discovery, pairing, wake-up, and streaming | Implemented; more real-hardware coverage needed |
| Local PS4 discovery, pairing, wake-up, and streaming | Implemented; more real-hardware coverage needed |
| 720p / 1080p / 1080p HQ | Available for Xbox and PlayStation sessions |
| Xbox Remote Play over the internet | Experimental; requires a direct network path |

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

1. Download `LunarNX.nro` from the [Releases page](https://github.com/thinkzhou/LunarNX/releases).
2. Copy it to the SD card:

   ```text
   sdmc:/switch/LunarNX/LunarNX.nro
   ```

3. Open Homebrew Menu using title override, then launch LunarNX.

Do not share the contents of `sdmc:/switch/LunarNX/`. It may contain Microsoft tokens, PlayStation credentials, PSN session data, console identifiers, and diagnostic logs.

## Getting Started

LunarNX opens with a platform selector. Choose Xbox or PlayStation.

### Xbox

1. Start Microsoft sign-in.
2. Open the displayed device-login URL on a phone or computer and enter the code.
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

1. Open the PlayStation page and sign in to PlayStation Network.
2. Refresh the PSN device list.
3. Select a PS5 with Remote Play enabled.
4. Wait while LunarNX creates the PSN session, establishes the control and data paths, and starts the Chiaki streaming session.

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

- Swipe left from the right edge of the touch screen to open the stream menu.
- The menu shows performance information and uses the correct **Xbox button** or **PS button** label for the active platform.
- Press **L + R + Plus** to send the platform Guide/PS button.
- To stop streaming with buttons, press **Minus + Plus together twice within three seconds**.
- The touch menu also requires a second confirmation before disconnecting.

## PlayStation Notes

- PlayStation protocol code is isolated under `src/ps/` and uses [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng).
- Remote PS5 streaming uses PlayStation Network signaling and Chiaki hole punching. Restrictive NAT, firewall rules, Wi-Fi loss, or PSN service behavior can still prevent a connection.
- The current video path uses H.264. Requested resolution and bitrate come from the shared LunarNX quality profile.
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

## Community, Support, And Privacy

- LunarNX QQ group: **736743823**
- Issues and releases: [GitHub repository](https://github.com/thinkzhou/LunarNX)

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

LunarNX builds on [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng), [libpeer](https://github.com/sepfy/libpeer), [Borealis](https://github.com/XITRIX/borealis), [FFmpeg](https://github.com/FFmpeg/FFmpeg), [libnx](https://github.com/switchbrew/libnx), [deko3d](https://github.com/devkitPro/deko3d), [Moonlight-Switch](https://github.com/XITRIX/Moonlight-Switch), and other projects listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Original LunarNX code outside the PlayStation module is available under the MIT License. PlayStation support links against chiaki-ng under AGPL-3.0. A combined binary containing the PlayStation path must be distributed under AGPL-3.0 terms with the complete corresponding source code and applicable dependency modifications available. See [LICENSE](LICENSE), [LICENSES/AGPL-3.0.txt](LICENSES/AGPL-3.0.txt), and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

LunarNX is not affiliated with, authorized by, or endorsed by Microsoft, Xbox, Sony Interactive Entertainment, PlayStation, Nintendo, or the maintainers of the referenced projects. All product names and trademarks belong to their respective owners.
