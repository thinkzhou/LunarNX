# Experimental libdatachannel SDK

The Switch app can link both its existing legacy libpeer and this optional
network backend. The runtime default remains libpeer. PlayStation still uses
Chiaki; libdatachannel is an Xbox/xCloud WebRTC experiment.

Sources are downloaded into the ignored `build/libdatachannel-sdk/` directory.
`prepare.py` pins and verifies upstream archive hashes and the MPL-licensed
Horizon adaptations from YOUYOU commit
`d9b640f320227dcd3857c6ad2947234d39a0a2b3`. Original dependency copyright and
license notices remain in those files. The component revisions and licenses
are in the pinned reference's `scripts/sources.lock.json` and `THIRD_PARTY.md`.
No UU authentication material or application protocol/UI code is built.

- libdatachannel: `9e6a13abbb6846c003d817d0387b6706466e2b03`, MPL-2.0.
- libjuice: `b509b6c1f90e4a1d4753e6bde62ffbeb4d10c6bb`, MPL-2.0.
- plog: `94899e0b926ac1b0f4750bfbd495167b4a6ae9ef`, MIT.
- Native-only vendored usrsctp: `fec583d54493f879d2ae44a743423bf8a04371ab`, BSD.

The Switch SDK reuses LunarNX's existing mbedTLS 3, libSRTP, and usrsctp
archives and headers. This avoids duplicate crypto/SCTP implementations in one
binary. Private C names shared by libjuice and libpeer are prefixed at compile
time. The imported thread-cleanup call sites use LunarNX's small owned-thread
implementation; UU application diagnostics are removed. Optional UU media
dispatch/timestamp experiments remain disabled.

Switch libraries **must** be built inside `devkitpro/devkita64:20251117`:

```sh
# Inside the same Docker environment as the regular NRO build:
bash tools/libdatachannel_switch/build.sh switch
make -f Makefile.switch -j6 CURL_PROVIDER=moonlight
```

`EXPERIMENTAL_WEBRTC=1` is the default Switch build. Set it to `0` to omit the
SDK and backend selector entirely. Changing build feature flags requires a
clean application rebuild. Both variants retain `WEBRTC_PROVIDER=legacy`.
The normal make dependency prepares the SDK before compiling application files.

For native development only:

```sh
bash tools/libdatachannel_switch/build.sh native
make -f Makefile.desktop EXPERIMENTAL_WEBRTC=1
```

The native recipe requires the normal LunarNX native mbedTLS/libSRTP archives.
The existing combined Xbox/PlayStation binary distribution terms remain
AGPL-3.0; keep all third-party notices and provide corresponding source.

The artifact packaging step runs `package_licenses.sh`, including the MPL-2.0,
plog MIT and usrsctp BSD texts plus source provenance and the hash-locked source
reconstruction script. See `THIRD_PARTY_NOTICES.md` in the package for component
attribution and source locations.

The package's `sources/LunarNX-revision.txt` identifies the exact application
source tree. Check out that tree before using the included preparation script;
the script copy is a source record, not a standalone application build kit.
