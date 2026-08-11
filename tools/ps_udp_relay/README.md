# Ryubing PlayStation UDP Relay

This development helper owns the UDP NAT mappings used by PSN Remote Play when
LunarNX runs under Ryubing. It does not receive PSN credentials and does not
parse Chiaki payloads.

Start it on the Mac address reachable from Ryubing. On current macOS Ryubing
setups this is normally the active `bridge100` address:

```sh
python3 tools/ps_udp_relay/ps_udp_relay.py \
  --listen-address 192.168.139.3 \
  --control-port 47998 \
  --secret 'replace-with-a-random-development-secret'
```

The control channel and the CTRL/DATA tunnels all use UDP. Each Chiaki
holepunch session supplies a random connection ID, so retries and parallel
clients cannot replace each other's channels. Control requests are
authenticated with a shared-secret HMAC and use request IDs, retries, and
deduplicated responses so a lost `open` response cannot replace a NAT mapping.
Allow incoming UDP for Python in the macOS firewall. Do not expose the helper
to the public internet. Configure the same address, port, and secret in
LunarNX. The relay uses matching Chiaki holepunch request/response IDs to lock
one exact PS5 endpoint, then forwards later datagrams unchanged. It does not
parse RUDP, registration, session, or media packets. Normal logs contain
endpoints and counters, never packet contents or
the shared secret.

This helper is only active when `ps_network_profile` is exactly `ryubing`.
Real Switch hardware should use the default `native_switch` profile.

## Native end-to-end gate

Before testing Ryubing, build the macOS Chiaki probe and require a complete
relay-assisted stream. The probe stores redacted diagnostics under `/tmp`; it
does not save credentials or media payloads.

```sh
probe=$(tools/ps_udp_relay/build_native_probe.sh)
export LUNARNX_PS_RELAY_HOST=192.168.139.3
export LUNARNX_PS_RELAY_PORT=47998
export LUNARNX_PS_RELAY_SECRET='replace-with-the-development-secret'
python3 tools/psn_remote_probe.py \
  --full --require-complete \
  --token "$HOME/work/self/ryujinx-data/sdcard/switch/LunarNX/psn_token.json" \
  --native-binary "$probe" \
  --device PS5-231 \
  --media-seconds 2
```

The gate fails unless CTRL registration, session startup, the DATA hole,
connection, H.264, Opus, zero frame loss, and clean shutdown all complete in
order. Runtime `status` responses contain only endpoint and packet-counter
diagnostics for the requesting connection ID.

## Ryubing UDP probe

Before debugging PSN, the standalone echo helper can verify the emulator's
non-blocking guest-to-Mac UDP path without credentials or UI interaction:

```sh
python3 tools/ps_udp_relay/udp_echo_probe.py \
  --listen-address 192.168.139.3 \
  --port 48000
```

Build `LunarNXNetProbe.nro` with matching `NETPROBE_UDP_HOST` and
`NETPROBE_UDP_PORT` values. The log is written to
`sdmc:/switch/LunarNXNetProbe/netprobe.log`.
