# Ryubing PlayStation UDP Relay Design

## Goal

Allow LunarNX running in Ryubing to complete PSN Remote Play CTRL and DATA
hole punching by delegating only UDP socket ownership and forwarding to a
macOS host helper. PSN HTTP, WebSocket signaling, credentials, Chiaki session
state, RUDP, decoding, rendering, audio, and input remain inside LunarNX.

The relay is a development-only path selected by
`ps_network_profile: "ryubing"`. Native Switch behavior must remain unchanged.

## Motivation

Ryubing successfully creates the PSN remote session and receives the PS5
OFFER, but its guest UDP/NAT implementation does not provide a stable external
mapping that receives PS5 traffic. It also sometimes returns a zero address
family from `recvfrom()`. Port guessing cannot repair a mapping owned by the
emulator's virtual NAT.

## Architecture

LunarNX connects to a trusted helper on the Mac LAN address. The helper owns
one external UDP socket per Chiaki channel:

- CTRL socket during the initial hole punch and subsequent control RUDP.
- DATA socket when Chiaki starts the data hole punch and subsequent media and
  input traffic.

Each channel also has a local UDP tunnel endpoint used by LunarNX. Datagram
boundaries are preserved. The helper does not parse or modify Chiaki payloads.

The control protocol uses a length-prefixed authenticated TCP connection. It
supports opening a channel, requesting STUN discovery, installing PS5
candidates, querying status, and closing a channel. Messages have a version,
request identifier, channel identifier, command, and payload. The configured
shared secret authenticates the first request using HMAC-SHA256 with a fresh
server nonce. Secrets and PSN credentials are never logged.

## Connection Flow

1. LunarNX reads the Ryubing relay host, TCP port, and shared secret from its
   config. Missing or invalid relay configuration is a visible connection
   error.
2. It authenticates to the Mac helper and opens the CTRL channel.
3. The helper creates and binds an external IPv4 UDP socket, performs STUN on
   that same socket, and returns the mapped address and port.
4. LunarNX uses the helper's mapped endpoint in its PSN CTRL offer instead of
   the Ryubing guest mapping.
5. After the PS5 OFFER arrives, LunarNX passes its candidates to the helper.
   The helper sends the normal 88-byte Chiaki probes through its external
   socket and locks onto the first peer that returns a protocol-valid packet.
6. The helper forwards subsequent external datagrams to LunarNX and forwards
   guest datagrams to the selected PS5 peer without changing boundaries.
7. When Chiaki requests DATA, the same sequence is repeated with an independent
   helper socket. Both sockets remain alive until session shutdown.

The relay must use the same external socket for STUN, candidate probing, and
steady-state traffic. This is required for NAT mapping continuity.

## Integration Boundary

The tracked chiaki-ng Switch patch gains a narrow relay transport hook. Socket
creation, send, receive, poll readiness, connect semantics, and close continue
to look like datagram operations to the hole-punch/session code. The hook is
enabled only when LunarNX supplies a relay configuration before creating the
Chiaki hole-punch session.

The native path does not call the hook and continues using libnx sockets.
LunarNX retains ChiakiSession ownership rules: the remote connector hands the
hole-punch session to ChiakiSession after CTRL succeeds, and ChiakiSession owns
DATA setup and final cleanup.

## Configuration

The Ryubing profile accepts:

```json
{
  "ps_network_profile": "ryubing",
  "ps_relay_host": "192.168.71.1",
  "ps_relay_port": 47998,
  "ps_relay_secret": "random-development-secret"
}
```

The host must be an explicit IPv4 address reachable from the emulator guest.
There is no automatic LAN discovery in the first version. The helper binds its
TCP and guest-facing UDP sockets only to the configured LAN address by default.

## Helper CLI

`tools/ps_udp_relay/ps_udp_relay.py` provides:

```text
--listen-address ADDRESS
--control-port PORT
--secret SECRET
--stun-server HOST:PORT
--idle-timeout SECONDS
--verbose
```

Normal output reports channel lifecycle, mapped endpoints, selected peer, byte
and packet counts, and sanitized errors. Packet contents, credentials, shared
secrets, and full PSN identifiers are not logged.

Only one LunarNX session is required initially. A second authenticated client
is rejected while the first is active.

## Failure Handling

- Helper unreachable or authentication failure: fail before PSN session
  creation with a specific relay error.
- STUN timeout: try the configured fallback list, then fail with the attempted
  server names.
- No PS5 candidate responds: report CTRL or DATA relay candidate timeout.
- Helper disconnect during streaming: surface a network error and close both
  channels.
- Cancel: close relay channels before finalizing the hole-punch session.
- Idle helper channels expire and close their sockets.

UI status must advance through relay connection, mapped endpoint acquisition,
PSN session creation, CTRL candidate probing, and CTRL establishment. It must
not remain on `Creating remote session` while candidate probing is in progress.

## Testing

Host tests use fake STUN and fake PS UDP endpoints to verify:

- authentication success and rejection;
- mapped endpoint is obtained from the same socket later used for forwarding;
- CTRL and DATA channel isolation;
- bidirectional forwarding with preserved datagram boundaries;
- candidate selection and unexpected-source rejection;
- timeout, cancellation, disconnect, and idle cleanup;
- no secret or payload leakage in normal logs.

Source regressions verify that native Switch does not enable relay hooks, the
remote connector preserves Chiaki ownership/order, and the tracked chiaki-ng
patch includes the relay integration. Completion also requires the focused PS
tests, clean Docker Switch build, ABI check, NRO BSS check, `git diff --check`,
and one interactive Ryubing PSN connection attempt with the helper running.

## Security And Scope

This is a LAN development tool, not an internet relay service. It has no PSN
token and cannot initiate PSN signaling. HMAC authentication prevents accidental
use by unrelated LAN clients, but traffic is not encrypted beyond the protocol
protection already present in Chiaki. The helper must not bind publicly unless
the developer explicitly requests it.

Out of scope:

- TURN or a general-purpose relay service;
- multi-user hosting;
- automatic Mac discovery;
- changing Ryubing itself;
- replacing native Switch hole punching;
- moving Chiaki session, media decoding, or input processing to macOS.
