# Switch FFmpeg build

This build follows wiliwili's FFmpeg 7.1 package at pinned revisions and adds
one LunarNX patch for NVDEC status-buffer initialization.

Ryubing does not always overwrite the complete NVDEC status area. Without
initialization, `ff_nvtegra_wait_decode()` can read stale `error_status` or
`mbs_in_error` values and return `AVERROR_UNKNOWN` on alternating frames.
Retrying those packets corrupts FFmpeg decoder state and causes visible
ghosting.

`nvtegra-status-clear.patch` clears only the mapped status range before frame
submission. Real hardware still overwrites this area, so genuine NVDEC errors
remain observable after the fence completes.

Build in the pinned Docker image:

```sh
./tools/ffmpeg_switch_build/build.sh
```

On Apple Silicon, Docker's amd64 translation can occasionally fail while many
compiler processes start concurrently. The build is resumable; reduce
parallelism and run it again:

```sh
FFMPEG_JOBS=4 ./tools/ffmpeg_switch_build/build.sh
```

Output is written under `build/ffmpeg-switch/install`. To also replace the
tracked project `libavcodec.a` after a successful build:

```sh
./tools/ffmpeg_switch_build/build.sh --install
```

To link the isolated build without replacing project libraries:

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    source /opt/devkitpro/switchvars.sh
    make -f Makefile.switch -j$(nproc) \
      FFMPEG_LIBDIR=build/ffmpeg-switch/install/lib \
      FFMPEG_SWSCALE_LIB=build/ffmpeg-switch/install/lib/libswscale.a
  '
```

Run the static regression check with:

```sh
python3 tests/ffmpeg_nvtegra_status_clear_test.py
```
