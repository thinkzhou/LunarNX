# Chiaki Transport Probe

This macOS-native probe exercises the pinned Chiaki reorder queue, frame
processor, and Jerasure FEC implementation without a console or simulator. Run:

```sh
tools/chiaki_transport_probe/run_macos.sh
```

The reorder scenario delays the first packet of the 94-unit burst observed in
the Switch hardware log and compares the upstream 64-packet capacity with the
LunarNX Switch capacity of 256. Frame scenarios measure complete frame assembly
with no loss and with recoverable source-unit loss from roughly 45 Kbit through
246 Kbit encoded frames.

Results are host CPU baselines. They can identify algorithmic discontinuities
and compare Chiaki revisions, but they do not measure libnx socket buffering,
Switch scheduling, Wi-Fi behavior, or Tegra CPU time. Real Switch testing is
still required for those platform boundaries.
