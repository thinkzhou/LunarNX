# Chiaki Switch SDK

`build_in_docker.sh` rebuilds Akira's pinned `github_repos/chiaki-ng-fork` and installs its archive,
public headers, generated `config.h`, and LunarNX ABI fingerprint as one SDK.

Run it from the LunarNX root. It uses `devkitpro/devkita64:20251117` unless
`LUNARNX_DEVKIT_IMAGE` is set. The source checkout must be Akira's commit
`1597a48514e5d9e67168ca40e6fa40c0171cd379`. The checkout remains clean. The
temporary build copy receives the tracked Switch patches. The STUN patch uses
the real-hardware-tested ordered IPv4 STUN list instead of the GitHub-hosted
dynamic list. The stream-switch patch handles a PSN console sending the CTRL
switch ACK while Senkusha is still waiting for BANG, and adds targeted Takion
sequence diagnostics. The focused hole-punch reliability patch removes consumed
ACK notifications, retries only a timed-out short ACK once with the same request
ID, checks OFFER delivery, gives DATA candidate selection three
cancellation-aware attempts, and reports exhausted DATA setup as a stream
connection error. The focused stream RTT patch measures live Takion DATA_ACK
round trips with the monotonic clock, keeps a fixed eight-sample window, and
uses the console's connection-quality RTT only until an ACK sample arrives.
The focused receive-buffer patch keeps Takion's protocol receive window intact
while increasing the Switch UDP socket buffer to 512 KiB in both LAN and PSN
socket paths. This gives 1080p high-bitrate bursts enough scheduling headroom
without changing the behavior of other Chiaki platforms.
The fork's native libnx crypto backend is used directly.

The pinned devkitA64 image does not include `protoc` or the Python protobuf
package. Until the toolchain image provides them, the checkout must also have
`pbgen/takion.pb`, `pbgen/takion.pb.c`, and `pbgen/takion.pb.h`; the build script
uses these generated inputs without regenerating them.

Akira's fork supplies the Switch crypto implementation. No LunarNX protocol,
crypto, relay, or packet-format changes are applied. The optional Ryubing UDP
relay profile remains unavailable.

The installed archive must pass:

```sh
python3 tests/chiaki_switch_abi_test.py
```

Do not copy only `libchiaki.a` or only the headers. Both are an ABI unit.
The Switch link runs this ABI check again before producing `LunarNX.elf`.
