# libdatachannel Switch/devkitPro Spike

Status: DONE_WITH_CONCERNS

## Scope

Evaluated `github_repos/libdatachannel` for Nintendo Switch/devkitPro cross-compilation. No LunarNX main build entry was integrated, and no `lib/libpeer`, `src/webrtc`, or `src/app` files were edited by this spike.

## Source And Environment

- libdatachannel: `github_repos/libdatachannel`, commit `188ec93f`, version `0.24.5`.
- Submodules initialized with proxy after initial GitHub timeout:
  - `deps/libjuice` `v1.7.2`
  - `deps/libsrtp` `24b3bf8`
  - `deps/plog` `94899e0`
  - `deps/usrsctp` `fec583d`
  - `deps/json` `55f9368`
- Docker image: `devkitpro/devkita64:20251117`, image id `sha256:74422a2754fd9b7d9320e861e0daf246736cf92f3d959bd641b5dda335e186ce`.
- Toolchain observed in container:
  - `aarch64-none-elf-gcc (devkitA64 release 28) 15.1.0`
  - `cmake version 3.31.6`
  - installed Switch MbedTLS package is `2.28.10`.

## libdatachannel Options Identified

- TLS backend:
  - Default: OpenSSL.
  - `USE_MBEDTLS=ON`: MbedTLS, but libdatachannel requires MbedTLS >= 3.
  - `USE_GNUTLS=ON`: GnuTLS; conflicts with `USE_MBEDTLS`.
- ICE backend:
  - Default vendored `libjuice`.
  - `USE_NICE=ON`: system libnice/GLib path, not suitable for this image.
- Dependencies:
  - `usrsctp` is mandatory for SCTP/data channels.
  - `libjuice` is mandatory unless using libnice.
  - `libsrtp` is used when `NO_MEDIA=OFF`.
  - `plog` is vendored/header-only.
  - `json` is only relevant when building examples.
- Module switches:
  - `NO_WEBSOCKET=ON` works for reducing surface area.
  - `NO_MEDIA=ON` works for configure, but is not viable for replacing libpeer in LunarNX because Xbox streaming needs SRTP media.
  - `NO_EXAMPLES=ON` and `NO_TESTS=ON` are appropriate for Switch static-library probing.

## Commands Run

Submodules:

```bash
export PROXY_URL=http://proxy.example:3128
git -C github_repos/libdatachannel -c http.proxy=$PROXY_URL -c https.proxy=$PROXY_URL submodule update --init --recursive --jobs 4
```

Main reproducible spike command:

```bash
tools/libdatachannel_switch_spike/run_spike.sh
```

The wrapper runs:

```bash
docker run --rm --platform linux/amd64 \
  -e http_proxy=http://proxy.example:3128 \
  -e https_proxy=http://proxy.example:3128 \
  -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 \
  bash -lc ./tools/libdatachannel_switch_spike/run_inside_docker.sh
```

Individual dependency probes:

```bash
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work devkitpro/devkita64:20251117 bash -lc '
source /opt/devkitpro/switchvars.sh
build_dir=tools/libdatachannel_switch_spike/build/media_no_websocket_mbedtls3
log_dir=tools/libdatachannel_switch_spike/logs
for target in srtp2 juice-static usrsctp; do
  cmake --build "$build_dir" --target "$target" -j1 > "$log_dir/target_${target}.build.log" 2>&1
  printf "%s\t%s\n" "$target" "$?"
done
'
```

After adding the local `endian.h` compatibility header, `juice-static` was retried:

```bash
cmake --build tools/libdatachannel_switch_spike/build/media_no_websocket_mbedtls3 --target juice-static -j1
```

## Results

Initial portlibs-only attempt:

```text
no_media_no_websocket_mbedtls      configure=1
media_no_websocket_mbedtls         configure=1
media_websocket_mbedtls            configure=1
no_media_no_websocket_openssl      configure=1
```

Reasons:

- MbedTLS path failed because devkitPro provides MbedTLS `2.28.10`; libdatachannel requires MbedTLS >= 3.
- OpenSSL path failed because Switch OpenSSL was not present in the image/package search.

MbedTLS 3 probe:

- Built MbedTLS `mbedtls-3.6.6` inside Docker only.
- Required non-default config for Switch:
  - `MBEDTLS_NO_PLATFORM_ENTROPY`
  - unset `MBEDTLS_HAVE_TIME_DATE`
  - unset `MBEDTLS_HAVE_TIME`
  - unset `MBEDTLS_TIMING_C`
  - unset `MBEDTLS_NET_C`
  - set `MBEDTLS_SSL_DTLS_SRTP`
- This installed successfully in the spike build, but it is not a complete runtime solution: Switch needs a real entropy source and likely a time hook.

libdatachannel with MbedTLS 3:

```text
dependency_mbedtls3                configure=0 build=0
no_media_no_websocket_mbedtls3     configure=0 build=1
media_no_websocket_mbedtls3        configure=0 build=1
media_websocket_mbedtls3           configure=0 build=1
no_media_no_websocket_openssl      configure=1
```

Dependency target probes:

```text
srtp2        build=0
juice-static build=0 after local endian.h compatibility header
usrsctp      build=1
```

## Error Summary

Primary blockers found:

- `ifaddrs.h` is missing from libnx/devkitA64. libdatachannel and libjuice include/use `getifaddrs/freeifaddrs` for interface/local candidate enumeration.
- `usrsctp` hits deeper BSD/POSIX assumptions:
  - missing BSD typedefs such as `u_long`, `u_int`, `u_short`
  - missing `netinet/in_systm.h`
  - `sys/uio.h` is not available as a normal header; libnx has `sys/_iovec.h` via `sys/socket.h`, so a final fix needs careful header adaptation rather than the crude spike stub.
- `libjuice` additionally needed `<endian.h>`; a spike-only wrapper to `<sys/endian.h>` was enough for `juice-static` to compile.
- MbedTLS 3 requires Switch-specific configuration:
  - default entropy, time, timing, and net helper modules assume Unix/Windows.
  - disabling them allowed compile, but production needs entropy/time integration.
- One no-media run also hit a Docker/Rosetta emulation trap while compiling a C++ object. The media builds reached deterministic usrsctp failures, so the main blocker assessment is usrsctp/platform API, not that transient emulator message.

What did work:

- libdatachannel CMake configure can succeed for Switch with vendored deps once MbedTLS 3 is provided and CMake is forced away from MbedTLS's mismatched config target names.
- vendored `libsrtp` built successfully with the custom MbedTLS 3.
- vendored `libjuice` built successfully after the local `endian.h` wrapper, though it still needs real `getifaddrs/freeifaddrs` runtime support for useful ICE host candidates.

## Recommendation

Do not replace libpeer with libdatachannel in the short term.

The minimal path to make libdatachannel viable is:

1. Create a Switch MbedTLS 3 port/package with `MBEDTLS_SSL_DTLS_SRTP`, a real libnx entropy source, and a deliberate time policy.
2. Port or replace usrsctp for devkitPro/libnx. This is the largest blocker: the current vendored usrsctp assumes BSD/POSIX headers and types that libnx does not provide directly.
3. Provide Switch implementations or libdatachannel/libjuice patches for interface enumeration (`getifaddrs/freeifaddrs`) and endianness includes.
4. Keep `NO_WEBSOCKET=ON` unless LunarNX explicitly needs libdatachannel WebSocket support.
5. Keep `NO_MEDIA=OFF` for any real libpeer replacement, because Xbox streaming needs SRTP media.

Until usrsctp is ported and MbedTLS runtime hooks are real, libdatachannel is not a drop-in or low-risk replacement for the current libpeer path.

## Files Written By This Spike

- `tools/libdatachannel_switch_spike/.gitignore`
- `tools/libdatachannel_switch_spike/run_spike.sh`
- `tools/libdatachannel_switch_spike/run_inside_docker.sh`
- `tools/libdatachannel_switch_spike/compat/include/endian.h`
- `tools/libdatachannel_switch_spike/compat/include/ifaddrs.h`
- `tools/libdatachannel_switch_spike/compat/include/switch_spike_compat.h`
- `tools/libdatachannel_switch_spike/compat/include/sys/uio.h`
- `tools/libdatachannel_switch_spike/attempt_summary.tsv`
- `tools/libdatachannel_switch_spike/attempt_summary_initial_portlibs.tsv`
- `tools/libdatachannel_switch_spike/logs/*.log`
- `tools/libdatachannel_switch_spike/report.md`

Large generated `build/` and `install/` directories were removed after logging and are ignored by the spike `.gitignore`.
