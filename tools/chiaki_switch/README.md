# Chiaki Switch SDK

LunarNX builds the pinned [xlanor/chiaki-ng](https://github.com/xlanor/chiaki-ng)
revision `907cd8219170b771a7d5d052fa8234b545b25a9f`. We selected it after
comparing the PS5 Takion v15 path with [Akira](https://github.com/xlanor/akira),
which pins the same revision in its Chiaki submodule. Akira is a reference
project, not a bundled LunarNX application dependency.
`github_repos/chiaki-ng-fork` is an ignored local checkout, not a Git
submodule or a directory committed to LunarNX. Prepare it with:

```sh
./scripts/setup_chiaki_dependencies.sh
./tools/chiaki_switch/build_in_docker.sh
```

The setup script checks that an existing checkout has no local changes before
moving it to the pinned commit. The Docker builder uses
`devkitpro/devkita64:20251117` by default. CI runs the same setup and library
builder before the LunarNX NRO build. For a source checkout elsewhere, set
`CHIAKI_SOURCE_CHECKOUT=/absolute/path` when invoking the Docker builder.

The image does not include protoc or the Python protobuf package. The tracked
`pbgen_v15` outputs match this revision's `lib/protobuf/takion.proto` and let
the container build without generating protobuf code. The build copies the
clean checkout to a temporary directory, applies the Switch-only video reorder
capacity and UDP receive-buffer patches, then installs `libchiaki.a`, its
dependent archives, public headers, generated `config.h`, and the LunarNX ABI
fingerprint together. Do not replace only the archive or only the headers.

The other tracked `lunarnx-chiaki-*.patch` files target the previous Chiaki
revision and are retained as historical references; the v15 builder does not
apply them. In particular, PSN route-preference and transport-diagnostic
patches have not been rebased onto v15's rewritten Takion/hole-punch code.
PSN Remote still builds against the upstream implementation, but needs real
hardware validation after this upgrade. There is no automatic HEVC-to-H.264
fallback.

The installed archive must pass:

```sh
python3 tests/chiaki_switch_abi_test.py
```

The Switch link runs the ABI check again before producing `LunarNX.elf`.
