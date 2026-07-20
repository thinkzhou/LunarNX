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
| Opus | <https://opus-codec.org/> | BSD-3-Clause |
| curl | <https://curl.se/> | curl license |
| zlib | <https://zlib.net/> | zlib |
| zstd | <https://github.com/facebook/zstd> | BSD-3-Clause/GPL-2.0-only dual license |

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

Protocol and platform behavior was studied using XStreaming, Greenlight,
xbox-xcloud-player, libnxbox, Moonlight-Switch, and wiliwili. LunarNX does not
include their trademarks and is not endorsed by their maintainers.
