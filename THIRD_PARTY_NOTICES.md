# Third-Party Notices

LunarNX includes, links against, or derives implementation guidance from
third-party open-source projects. Each component remains subject to its own
license; the LunarNX MIT license does not replace those terms.

## Runtime and build dependencies

| Component | Project | License family |
| --- | --- | --- |
| libpeer | <https://github.com/sepfy/libpeer> | MIT |
| Borealis (XITRIX fork) | <https://github.com/XITRIX/borealis> | MIT |
| FFmpeg with Nintendo Switch NVDEC patches | <https://github.com/FFmpeg/FFmpeg> and <https://github.com/xfangfang/wiliwili> | LGPL/GPL, depending on configuration |
| libnx | <https://github.com/switchbrew/libnx> | ISC |
| deko3d | <https://github.com/devkitPro/deko3d> | zlib |
| Mbed TLS | <https://github.com/Mbed-TLS/mbedtls> | Apache-2.0 or GPL-2.0-or-later, depending on version |
| libsrtp | <https://github.com/cisco/libsrtp> | BSD-3-Clause |
| cJSON | <https://github.com/DaveGamble/cJSON> | MIT |
| QR Code generator library | <https://github.com/nayuki/QR-Code-generator> | MIT |
| Opus | <https://opus-codec.org/> | BSD-3-Clause |
| curl 8.11.x | <https://curl.se/> | curl license |
| zlib | <https://zlib.net/> | zlib |
| zstd | <https://github.com/facebook/zstd> | BSD-3-Clause/GPL-2.0-only dual license |
| chiaki-ng 1.10 | <https://github.com/chiaki-ng/chiaki-ng> | AGPL-3.0 |
| gf-complete | (chiaki dependency) | BSD-3-Clause |
| jerasure | (chiaki dependency) | BSD-3-Clause |
| protobuf-nanopb | (chiaki dependency) | zlib |
| mbedTLS 3.4.0 | <https://github.com/Mbed-TLS/mbedtls> | Apache-2.0 or GPL-2.0-or-later |

The exact source revisions for Borealis and legacy libpeer are pinned in
`scripts/setup_dependencies.sh`. The FFmpeg revision, downloaded patches, and
checksums are pinned in `tools/ffmpeg_switch_build/build_in_docker.sh`.

## Binary distribution warning

The current Switch FFmpeg build script passes `--enable-gpl` and
`--enable-version3`. Anyone distributing a linked `LunarNX.nro` must determine
the resulting license for that exact build and satisfy the corresponding
source-code, notice, relinking, and license-copy obligations for every linked
component. A GitHub source release alone does not automatically satisfy the
requirements for a binary release.

Before publishing an NRO release, archive the exact dependency source and
patches used for that artifact, include the applicable license texts, and make
the corresponding source available from the release page. This file is an
engineering inventory, not legal advice.

## Referenced projects

The following projects were studied for protocol behavior, interoperability,
platform integration, or client workflows. Listing a project here does not mean
that its source code is bundled or linked into LunarNX; runtime and build
dependencies are listed separately above.

| Project | License | Reference area |
| --- | --- | --- |
| [XStreaming](https://github.com/Geocld/XStreaming) | MIT | Xbox Remote Play, Xbox Cloud Gaming, signaling, ICE, input, and vibration behavior |
| [PeaSyo](https://github.com/Geocld/PeaSyo) | AGPL-3.0 | PlayStation Remote Play workflows and connectivity behavior |
| [Greenlight](https://github.com/unknownskl/greenlight) | See upstream project | Xbox authentication and streaming behavior |
| [xbox-xcloud-player](https://github.com/unknownskl/xbox-xcloud-player) | See upstream project | Xbox Cloud Gaming WebRTC behavior |
| [libnxbox](https://github.com/ursusworks/libnxbox) | See upstream project | Nintendo Switch Xbox streaming integration |
| [Moonlight-Switch](https://github.com/XITRIX/Moonlight-Switch) | See upstream project | Nintendo Switch media, audio, rendering, and UI integration |
| [wiliwili](https://github.com/xfangfang/wiliwili) | See upstream project | Nintendo Switch FFmpeg and NVDEC integration |

LunarNX does not include these projects' trademarks and is not endorsed by
their maintainers.

## Platform marks

The monochrome Xbox and PlayStation glyph source paths under `res/platform/`
were obtained from the Simple Icons project (<https://simpleicons.org/>). Xbox
and the Xbox Sphere mark are trademarks of Microsoft. PlayStation and the
PlayStation family mark are trademarks of Sony Interactive Entertainment.
Their appearance identifies supported platforms and does not imply endorsement
of LunarNX by either trademark owner.
