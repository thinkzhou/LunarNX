# Ryubing PlayStation UDP Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route Ryubing PlayStation CTRL and DATA UDP through a Mac-owned NAT mapping while leaving PSN signaling and Chiaki protocol processing in LunarNX.

**Architecture:** A Python host helper owns one public UDP socket per channel and exposes an authenticated length-prefixed TCP control API. The tracked chiaki-ng Switch patch opens relay channels while creating offers, substitutes the helper's STUN mapping, sends PS5 candidate lists to the helper, and connects the guest UDP socket to the helper for steady-state forwarding. The native Switch path is unchanged.

**Tech Stack:** Python 3 asyncio/socket/HMAC, C99 chiaki-ng/libnx sockets, C++20 LunarNX configuration, Python regression tests, Docker devkitA64.

---

### Task 1: Host Relay Protocol And Feedback Loop

**Files:**
- Create: `tools/ps_udp_relay/ps_udp_relay.py`
- Create: `tests/ps_udp_relay_test.py`
- Create: `tools/ps_udp_relay/README.md`

- [ ] Write a host integration test that starts fake STUN and PS UDP peers, authenticates a relay client, opens CTRL and DATA channels, checks the mapped endpoints, installs candidates, and verifies bidirectional datagram forwarding.
- [ ] Run `python3 tests/ps_udp_relay_test.py` and require it to fail because the helper does not exist.
- [ ] Implement a four-byte big-endian length-prefixed JSON protocol with `hello`, `open`, `peers`, `status`, `close`, and `shutdown` commands. Authenticate `hello` using HMAC-SHA256 over the server nonce.
- [ ] Implement one UDP socket per channel. Perform RFC 5389 binding discovery on the same socket, distinguish the registered guest endpoint from external peers, select the first peer sending an 88-byte Chiaki packet, and preserve datagram boundaries while forwarding.
- [ ] Add idle cleanup, one-client enforcement, sanitized logging, CLI validation, and documentation.
- [ ] Re-run `python3 tests/ps_udp_relay_test.py` and require all relay cases to pass.

### Task 2: Chiaki Relay Client And Offer Integration

**Files:**
- Modify: `github_repos/chiaki-ng/lib/include/chiaki/remote/holepunch.h`
- Modify: `github_repos/chiaki-ng/lib/src/remote/holepunch.c`
- Modify: `tools/chiaki_switch/lunarnx-chiaki-switch.patch`
- Modify: `tests/ps_switch_runtime_dependencies_test.py`

- [ ] Add failing source-contract assertions for relay configuration, CTRL/DATA channel selection, mapped candidate substitution, peer installation, native-path isolation, and cleanup.
- [ ] Run `python3 tests/ps_switch_runtime_dependencies_test.py` and require relay assertions to fail.
- [ ] Add `chiaki_holepunch_session_set_relay()` to copy an IPv4 host, control port, and secret into session-owned state before offer creation.
- [ ] Add a bounded relay TCP client in `holepunch.c`: framed JSON requests, nonce/HMAC authentication, strict response parsing, timeouts, and reconnect-safe cleanup.
- [ ] During CTRL and DATA offer creation, request a helper channel, bind the guest UDP socket, register its endpoint with the helper, and replace the advertised STUN candidate with the returned public mapping.
- [ ] Before candidate checking, serialize the PS5 IPv4 candidates to the helper. Send guest packets to the helper LAN UDP endpoint and accept helper responses as the selected derived candidate. Keep the connected guest socket alive for Chiaki's existing RUDP/data owners.
- [ ] Close helper channels from hole-punch session finalization. Keep every relay branch behind explicit session relay configuration.
- [ ] Regenerate `tools/chiaki_switch/lunarnx-chiaki-switch.patch`, verify it applies to commit `6547d8aed03503646fe1043512616e26c03fa9db`, rebuild the Chiaki SDK, and run the dependency and ABI tests.

### Task 3: LunarNX Configuration And Status

**Files:**
- Modify: `src/ps/ps_remote_connector.cpp`
- Modify: `config/default_config.json`
- Modify: `tests/ps_remote_flow_test.py`
- Modify: `docs/ryujinx_testing.md`

- [ ] Add failing tests requiring relay host, port, and secret validation only for the Ryubing profile and requiring relay setup before PSN session creation.
- [ ] Run `python3 tests/ps_remote_flow_test.py` and require the new assertions to fail.
- [ ] Parse `ps_relay_host`, `ps_relay_port`, and `ps_relay_secret`; reject missing, non-IPv4, out-of-range, or oversized values with a visible status/error before creating the PSN session.
- [ ] Call `chiaki_holepunch_session_set_relay()` immediately after session initialization and surface `Connecting to Mac UDP relay...`, `Creating PSN remote session...`, and `Punching control channel through relay...` status text.
- [ ] Document helper launch, Mac LAN address selection, config, firewall expectations, and sanitized troubleshooting.
- [ ] Run PS flow, auth, identity, page, launch, and dependency regressions.

### Task 4: Switch Build And Interactive Verification

**Files:**
- Verify: `build/switch/LunarNX.nro`
- Verify: `$HOME/work/self/ryujinx-data/sdcard/switch/LunarNX/lunarnx.log`

- [ ] Run the host fake STUN/PS integration test and all focused PS regressions.
- [ ] Clean-build the Switch NRO in `devkitpro/devkita64:20251117` with `CURL_PROVIDER=moonlight`.
- [ ] Run Chiaki ABI, NRO BSS, Xbox session, libpeer SCTP/DTLS, datachannel PPID, and `git diff --check` regressions.
- [ ] Start the helper on the Mac LAN address and launch Ryubing through LaunchServices.
- [ ] Have the user click Connect and require the log to show relay CTRL mapping, relay peer selection, `chiaki_session_init rc=0`, relay DATA mapping, and either first video or a later protocol-specific failure.
- [ ] Record remaining real-Switch risk; relay success is simulator development evidence only.
