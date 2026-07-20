# Switch LAN ICE Candidate Design

## Goal

Allow LunarNX to establish the Xbox WebRTC connection when the client and
console are on the same LAN, without waiting for unusable Teredo or public
NAT-hairpin candidate pairs to exhaust the DataChannel timeout.

## Observed Failure

The official Xbox session and signaling requests succeed, but ICE remains in
`checking` until `waitDataChannels()` times out after 15 seconds. In the
captured home-network case:

- the Ryubing host was `192.168.1.11`;
- the Xbox advertised `192.168.1.10:9002` and was directly reachable;
- LunarNX advertised only a server-reflexive public candidate;
- Teredo-derived public candidates were tried before the reachable LAN
  candidate.

The Switch compatibility `ifaddrs.h` in the legacy libpeer checkout returns an
empty interface list, so upstream `ports_get_host_addr()` cannot create a host
candidate. Legacy libpeer also checks candidate pairs sequentially and allows
up to 1000 checks per pair, which lets one unusable public pair consume the
entire application timeout.

## Design

### Switch Host Candidate Gathering

`lib/libpeer/src/ports.c` will use `nifmGetCurrentIpAddress()` under
`__SWITCH__` for IPv4 host-address discovery. The returned address will be
stored in the existing `Address` supplied by `agent_create_host_addr()` while
preserving the UDP socket's bound port.

The non-Switch `getifaddrs()` path remains unchanged. The ignored local
libpeer checkout is not committed directly; the same source change must be
represented in `tools/libpeer_legacy/legacy-libpeer-switch.patch` and described
in `tools/libpeer_legacy/README.md`.

If NIFM cannot provide a valid non-zero IPv4 address, host candidate gathering
fails softly and the existing STUN server-reflexive candidate remains
available.

### Remote Candidate Ordering

`IceCandidateProcessor` will order usable remote IPv4 candidates as follows:

1. RFC1918 host candidates (`10/8`, `172.16/12`, `192.168/16`).
2. Other original IPv4 candidates.
3. Teredo-derived public IPv4 candidates.
4. Native IPv6 candidates.
5. End-of-candidates markers.

Ordering is stable within each class. Duplicate and malformed candidates keep
the existing filtering behavior. This ordering lets legacy libpeer attempt a
direct Xbox LAN pair first while retaining public and IPv6 fallbacks.

The application does not require exact subnet knowledge for this first fix.
Private host candidates are preferred because they are only useful when local
routing exists; a failed private candidate still falls back to the remaining
candidate classes.

### Timeouts

`AGENT_CONNCHECK_MAX` and the 15-second application timeout will not change in
this patch. Candidate ordering and host discovery address the demonstrated
root cause without weakening ICE behavior on higher-latency networks. Timeout
tuning can be evaluated separately if evidence still shows candidate
starvation.

## Data Flow

1. libpeer binds its UDP socket.
2. `ports_get_host_addr()` obtains the Switch IPv4 address from NIFM and adds a
   host candidate using the bound UDP port.
3. STUN gathering adds the existing server-reflexive candidate.
4. LunarNX sends both candidates to the Xbox signaling API.
5. Xbox candidates are parsed, normalized, deduplicated, and ordered with LAN
   candidates first.
6. libpeer checks the direct LAN pair before public/Teredo fallbacks.
7. Successful ICE proceeds to DTLS, SCTP, and the four Xbox DataChannels.

## Verification

- Unit tests prove private IPv4 candidates precede Teredo-derived and public
  candidates while preserving all candidates.
- A source/patch regression test proves the Switch host-address path uses
  `nifmGetCurrentIpAddress()` and that the tracked patch contains the change.
- Existing ICE, session-order, SCTP, DTLS, and DataChannel regressions remain
  green.
- Docker/devkitA64 builds the Switch NRO with `WEBRTC_PROVIDER=legacy`.
- Ryubing official Xbox testing must show ICE transition from `checking` to
  `connected/completed` and DataChannel readiness, or capture the next failing
  protocol stage.
- Real Switch hardware remains the final acceptance target.

## Non-Goals

- Replacing legacy libpeer with upstream libpeer or libdatachannel.
- Adding a TURN service.
- Changing Xbox authentication, SDP payloads, media decode, or rendering.
- Treating Ryubing success as proof of real Switch compatibility.
