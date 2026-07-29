# Legacy libpeer Patch

LunarNX currently builds Switch WebRTC with the legacy local clone at
`lib/libpeer`. The directory is ignored by the main repository because it is an
embedded git checkout with generated build outputs and third-party dependency
trees.

Use `legacy-libpeer-switch.patch` to reproduce the local Switch/Xbox changes on
top of the libpeer checkout currently used by this project.

```sh
git clone https://github.com/sepfy/libpeer.git lib/libpeer
git -C lib/libpeer checkout bdc50f0cae13f19a31bb11827daea3a8354b173f
git -C lib/libpeer apply ../../tools/libpeer_legacy/legacy-libpeer-switch.patch
```

The patch includes the Switch/Xbox media-path fixes, including the inbound RTP
packet buffer increase from `CONFIG_MTU` to `CONFIG_PACKET_BUFFER_SIZE=2048`.
That avoids truncating aiortc H.264 RTP packets after SRTP overhead is added.
Decoded media callbacks also expose the source RTP sequence number and
timestamp. LunarNX uses those values for Opus reordering, packet-loss
concealment, and RTP-clock-based A/V synchronization.

The UDP socket requests a 4 MiB `SO_RCVBUF` as a best-effort burst cushion for
1080p/HQ IDR frames. `peer_connection_loop()` reports receive/decode work so
LunarNX can perform a bounded multi-pass drain without starving DTLS or SCTP;
the socket and drain changes are safe to ignore when a target clamps the
requested buffer size.

The completed-state loop intentionally retains main's packet-count limits
without adding an inner wall-clock deadline. The outer LunarNX pump remains
bounded, while one libpeer call can drain a burst already waiting in the UDP
socket instead of stopping after 2-3 ms. DTLS application writes still cap
WANT_READ/WANT_WRITE retries and propagate backpressure without advancing
custom SCTP TSN or stream-sequence state. LunarNX retains reliable startup
commands and PLI for a later owner-thread pump, replaces stale input, and
permits bounded NACK/feedback drops.

On Switch, libpeer obtains the local IPv4 host ICE candidate through
`nifmGetCurrentIpAddress()`. The compatibility `getifaddrs()` implementation
returns an empty list on this platform, which otherwise leaves only the public
STUN candidate and prevents reliable direct-LAN Xbox connections.

Each ICE session also generates and reuses a non-zero 64-bit tie-breaker. Xbox
and other strict ICE implementations may reject the fixed zero value used by
the legacy libpeer base revision.

Remote ICE candidates follow XStreaming's default (`ipv6=false`) expansion and
priority rules. Legacy libpeer then runs a paced ICE checklist instead of
retrying one candidate pair until timeout: checks are matched by STUN
transaction ID, candidate-pair priority uses the RFC ICE formula, and the
controlling client performs regular nomination only after connectivity has
been confirmed. This is required for remote play because the Xbox response can
list an unreachable private address before its Teredo-derived public address.
The candidate array holds 16 entries instead of upstream legacy libpeer's 10,
so XStreaming-compatible Teredo expansion does not truncate valid Xbox IPv6
fallback candidates.

SDP and trickle ICE use the same numeric BUNDLE mids as XStreaming's
react-native-webrtc path (`0` video, `1` audio, `2` data). Local candidates are
gathered while the offer is created but are not appended to the offer's final
media section; they are emitted only through the Xbox `/ice` exchange. The
offer advertises `trickle renomination`, and server-reflexive candidates carry
their real host candidate in `raddr/rport` instead of `0.0.0.0:0`.

Incoming ICE Binding requests retain `PRIORITY`, `USE-CANDIDATE`, and the
controlling/controlled tie-breaker attributes. Sources not present in the Xbox
signaling payload become peer-reflexive candidates and schedule a triggered
check. Remote nomination is completed only after the corresponding pair is
valid, and role conflicts switch roles or return a signed 487 response. Binding
success responses can likewise create a local peer-reflexive candidate when an
endpoint-dependent NAT reports a mapping different from the pre-gathered STUN
candidate.

Connectivity checks use a 500 ms initial RTO, exponential backoff capped at
8 seconds, and six retransmissions. LunarNX keeps the outer DataChannel wait
open for 45 seconds so it does not tear down the Xbox session while this ICE
window is still active.

The nominated ICE pair retains the measured Binding-response round-trip time
and exposes it through `PeerConnectionMediaStats::ice_rtt_ms`. Once streaming
is established, a one-second ICE consent check refreshes the same RTT sample
without reopening nomination. LunarNX therefore shows a live network RTT
instead of an HTTP request time, a fabricated latency value, or the frozen
RTT from the initial connectivity check.

Established connections use a 30-second liveness timeout instead of legacy
libpeer's 10-second default. Valid inbound SRTP, SRTCP, and DTLS traffic refreshes
the same liveness timestamp as an ICE Binding request, so a healthy xCloud media
flow is not closed merely because a periodic consent check is delayed by WAN or
simulator jitter.

LunarNX applies the remote SDP answer before posting local ICE, matching
XStreaming's signaling order. An empty candidate-pair checklist therefore
stays in the checking state until the subsequently exchanged trickle ICE
candidates arrive instead of failing immediately.

DTLS role negotiation also follows XStreaming's browser/libwebrtc behavior.
LunarNX offers `a=setup:actpass`; the Xbox answers `a=setup:passive`; LunarNX
then reconfigures the existing mbedTLS session as the active DTLS client. The
role change preserves the certificate and fingerprint already advertised in
the offer. Advertising `passive` in the offer leaves both LunarNX and the Xbox
waiting for a ClientHello and prevents ICE/DTLS/DataChannel completion.

WebRTC DTLS certificates are self-signed and authenticated by the SHA-256
fingerprint carried in SDP, not by PKIX CA, validity-time, name, or usage
checks. The mbedTLS verification callback therefore clears PKIX flags, while
`dtls_srtp_handshake()` still compares the peer certificate digest with the
negotiated remote fingerprint before marking DTLS connected. This is required
on real Switch hardware, whose mbedTLS time path can mark a newly generated
Xbox certificate `MBEDTLS_X509_BADCERT_FUTURE` even when the displayed times
differ by only seconds. Disabling the post-handshake fingerprint comparison is
not an acceptable workaround.

Incoming SRTP is kept in libpeer's bounded RTP queue while LunarNX initializes
the media pipeline and Xbox data channels. Decoding remains gated until the
pipeline is ready, but packets are not discarded. This preserves the Xbox's
startup SPS/PPS/IDR access unit; dropping RTP during that interval can leave
the decoder receiving only P-frames indefinitely.

If that startup queue reaches its limit while media is still gated, it keeps a
rolling, contiguous tail by evicting the oldest packet before appending the
newest one. Once media is enabled, LunarNX drains queued RTP before accepting
another full socket burst. This prevents client-side startup overflow from
appearing as thousands of network-lost packets in the performance HUD.

For video, legacy libpeer now exposes decrypted raw RTP packets instead of
depacketizing H.264 immediately. LunarNX applies a bounded, timestamp-ordered
jitter buffer that waits 60-180 ms based on the measured ICE RTT, reorders
late packets, and sends RFC 4585 Generic NACK feedback for short sequence gaps.
Incomplete or malformed access units are never passed to FFmpeg: after the
bounded wait expires, LunarNX requests a PLI and drops P-frames until a real
IDR arrives without blocking on a GPU-wide decoder drain. Receiver Reports use
the same wrap-aware sequence accounting, so a late retransmission repairs the
reported loss instead of permanently inflating it.

The video jitter path has hard heap limits of 32 frames, 2048 packets, 3 MiB of
buffered RTP payload, and 2 MiB per assembled H.264 access unit. Capacity or
allocation failures clear only this bounded buffer and enter keyframe recovery;
they do not allow an exception to escape the WebRTC callback. These buffers are
dynamic and do not increase the Switch NRO BSS image.

Receiver-side keyframe recovery matches the behavior supplied automatically by
XStreaming's libwebrtc. LunarNX sends the Xbox control-channel
`videoKeyframeRequested` message and an RFC 4585 RTCP PLI. Legacy libpeer's PLI
function was previously an unsent TODO; the patch now writes the RTCP header in
network byte order, protects it with the negotiated outbound SRTCP context, and
sends it through the selected ICE pair.

All outbound RTCP packets are copied into a 32-bit-aligned
`CONFIG_PACKET_BUFFER_SIZE` scratch buffer before SRTCP protection. The
encryption boundary also receives the buffer's actual capacity and rejects any
packet that cannot reserve `SRTP_MAX_TRAILER_LEN + 4` trailing bytes. This is
required because libsrtp protects RTCP in place; passing the exact 16-byte NACK
or 24-byte REMB stack array directly would overwrite the caller's stack when
libsrtp appends its authentication trailer.

Run `python3 tests/libpeer_pli_send_test.py` to verify that both the active
checkout and this tracked patch preserve the capacity contract. Run
`make -f Makefile.desktop srtcp_capacity_tests` for the executable canary test:
an exact-size 16-byte packet must be rejected without modifying adjacent stack
bytes.

The receiver also advertises its selected 720p/1080p/HQ capability through the
message-channel `clientdevicecapabilities` and `dimensionschanged` messages.
Once media is enabled, LunarNX sends a compound RTCP Receiver Report plus a
REMB roughly once per second. The report records the remote Sender Report's LSR
and computes DLSR in 1/65536-second units; using raw milliseconds here makes the
sender's RTT estimator overflow and can pin its encoder at a starvation bitrate.
The REMB value is the profile's receiver cap (10 Mbps for 720p, 20 Mbps for
1080p, or an explicitly selected 30 Mbps HQ profile), not a local encoder
setting.

Switch release builds default to `CONFIG_IPV6=0`; pass `IPV6=1` to the Switch
make invocation when testing native IPv6 paths. When enabled, IPv6 host
discovery uses a UDP route probe, filters unspecified, loopback, and link-local
addresses, and falls back to IPv4 when an IPv6 socket is unavailable. Peer
configuration uses XStreaming's seven default STUN URLs in the same order;
STUN URLs without an explicit port use port 3478.

Every synchronous STUN/TURN receive records the returned datagram length before
parsing, and STUN integrity validation initializes the same length explicitly.
The parser enforces that boundary; omitting it makes a valid Binding response
look empty and produces unusable `0.0.0.0:0` server-reflexive candidates.
Binding and Allocate responses must also match the request transaction ID and
contain a non-zero mapped or relayed address before a candidate is accepted.
Synchronous STUN gathering uses twenty 50 ms polls instead of one thousand
1 ms polls. The total one-second timeout is unchanged, while Ryubing avoids
thousands of BSD HLE calls. Normal ICE/media event polling remains at 1 ms.
