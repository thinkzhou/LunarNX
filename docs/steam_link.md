# Steam Link integration

The Switch build now contains a build-time selectable Steam Link entry point
(`STEAMLINK=1`, the default). The first vertical slice implements the part of
Steam Remote Play that can be tested independently of the media session:

- Steam host discovery through the Remote Play UDP broadcast protocol.
- Persistent client identity generation under `sdmc:/switch/LunarNX/`.
- Four-digit pairing-code authorization using [ihslib](https://github.com/mariotaku/ihslib).
- Switch UI for host selection, PIN entry, and authorization errors.

Build dependencies are pinned as Git submodules:

```sh
git submodule update --init --recursive
```

The current screen intentionally stops after pairing. Steam Remote Play's
stream transport is a separate protocol/session layer; it still needs an
adapter for ihslib's encrypted session frames, Steam's negotiated codecs, the
existing LunarNX decode/audio pipeline, and Switch controller input. Until that
adapter is implemented, the UI does not claim that a paired host can stream.

The pairing direction is important: LunarNX generates and displays the
four-digit pairing code, and the user enters that code in Steam's `Pair Steam
Link` dialog on the host. The host's optional fixed Remote Play security code
is a separate setting and is not used by this pairing screen.

The bundled ihslib sources are used only for the discovery/authorization slice
at present. Its session/HID sources are not linked yet. The ihslib dependency is
LGPL-3.0; keep its license and notices when distributing a build.

## Desktop discovery probe

The dependency-free probe can validate the network discovery path without a
Switch NRO:

```sh
python3 tools/steamlink_probe.py --target 255.255.255.255 --timeout 6
```

Use `--target <host-ip>` to avoid broadcast while testing a known host. The
probe parses the same discovery framing and status fields used by LunarNX.

For authorization, `tools/steamlink_probe/authorize.c` is a small POSIX
ihslib client. It must be compiled with the vendored ihslib sources and a
desktop mbedTLS build, then run with the host security code as its only
argument. Authorization should be performed only against a Steam host you
control; the PIN is passed on the local command line and is not stored.
