# Switch LAN ICE Candidates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make LunarNX establish ICE directly with an Xbox on the same LAN by advertising the Switch IPv4 host candidate and trying private Xbox candidates before Teredo/public fallbacks.

**Architecture:** Keep Xbox signaling and legacy libpeer intact. Add deterministic candidate classification in `IceCandidateProcessor`, and add a Switch-only NIFM branch to legacy libpeer host-address discovery. Track the ignored libpeer source change through the repository patch and a source-level regression test.

**Tech Stack:** C++20, C/libnx NIFM, legacy libpeer, Python source regressions, Docker devkitA64.

## Global Constraints

- Build all Switch artifacts with `devkitpro/devkita64:20251117`.
- Keep `WEBRTC_PROVIDER=legacy` and do not replace local libpeer.
- Do not build Switch libraries on macOS.
- Preserve public, Teredo, IPv6, and end-of-candidates fallbacks.
- Real Switch hardware remains the final acceptance target.

---

### Task 1: Prefer Reachable LAN Candidates

**Files:**
- Modify: `tests/ice_candidate_processor_test.cpp`
- Modify: `src/app/ice_candidate_processor.cpp`

**Interfaces:**
- Consumes: `IceCandidateProcessor::parseRemotePayload(const std::string&, const StreamProfile&)`
- Produces: a stable candidate list ordered private IPv4, original public IPv4, Teredo-derived IPv4, native IPv6, end marker.

- [ ] **Step 1: Write the failing ordering regression**

Change the existing Teredo test to assert that
`192.168.1.10:9002` is first, the two derived
`223.166.246.166` candidates remain present after it, and the end marker remains last.

- [ ] **Step 2: Run the regression and verify RED**

Run:

```sh
make -f Makefile.desktop stream_tests
```

Expected: FAIL because current code puts the Teredo-derived mapped-port candidate first.

- [ ] **Step 3: Implement stable candidate classification**

Replace `prioritizeTeredoDerivedCandidates()` with an ordering helper that:

```cpp
enum class CandidateClass {
    PrivateIpv4,
    PublicIpv4,
    TeredoDerivedIpv4,
    NativeIpv6,
    EndMarker,
};
```

Use parsed candidate addresses to recognize RFC1918 IPv4 ranges. Mark candidates produced by `deriveTeredoCandidates()` as derived rather than inferring only from their final public address. Append each class in order while continuing to use `appendCandidate()` for normalization and deduplication.

- [ ] **Step 4: Run the regression and verify GREEN**

Run:

```sh
make -f Makefile.desktop stream_tests
```

Expected: `ice candidate processor tests passed` and H.264 RTP regressions pass.

---

### Task 2: Gather the Switch Host IPv4 Candidate

**Files:**
- Create: `tests/libpeer_switch_host_candidate_test.py`
- Modify ignored dependency: `lib/libpeer/src/ports.c`
- Modify: `tools/libpeer_legacy/legacy-libpeer-switch.patch`
- Modify: `tools/libpeer_legacy/README.md`

**Interfaces:**
- Consumes: `int ports_get_host_addr(Address* addr, const char* iface_prefix)`
- Produces: the current Switch IPv4 address in `addr->sin.sin_addr.s_addr` while preserving the UDP socket family and port.

- [ ] **Step 1: Write the failing source/patch regression**

The Python test must assert that both the local ignored source and tracked patch contain:

```c
#ifdef __SWITCH__
u32 current_addr = 0;
Result rc = nifmGetCurrentIpAddress(&current_addr);
```

It must also assert a non-zero-address check, assignment to
`addr->sin.sin_addr.s_addr`, and that the non-Switch `getifaddrs()` path remains available.

- [ ] **Step 2: Run the regression and verify RED**

Run:

```sh
python3 tests/libpeer_switch_host_candidate_test.py
```

Expected: FAIL because neither the local source nor tracked patch uses NIFM.

- [ ] **Step 3: Implement the Switch-only NIFM path**

In `lib/libpeer/src/ports.c`, include `<switch.h>` under `__SWITCH__`. At the start of `ports_get_host_addr()`, handle IPv4 on Switch with `nifmGetCurrentIpAddress()`. Return `1` only for a successful, non-zero address; return `0` for IPv6 or unavailable NIFM data. Keep the existing LWIP and `getifaddrs()` branches unchanged for other platforms.

- [ ] **Step 4: Regenerate the tracked legacy patch**

Regenerate `tools/libpeer_legacy/legacy-libpeer-switch.patch` from the local checkout diff against `bdc50f0cae13f19a31bb11827daea3a8354b173f`. Update the README to state that Switch host ICE candidates use NIFM because `getifaddrs()` is unavailable.

- [ ] **Step 5: Run the regression and verify GREEN**

Run:

```sh
python3 tests/libpeer_switch_host_candidate_test.py
git -C lib/libpeer diff --check
git diff --check
```

Expected: all commands exit 0.

---

### Task 3: Verify the Full ICE and Switch Build Paths

**Files:**
- Verify only; no planned production changes.

**Interfaces:**
- Consumes: candidate ordering and NIFM host gathering from Tasks 1 and 2.
- Produces: a Docker-built official-auth NRO and an official Ryubing connection trace.

- [ ] **Step 1: Run focused regressions**

Run:

```sh
make -f Makefile.desktop stream_tests
python3 tests/libpeer_switch_host_candidate_test.py
python3 tests/xbox_stream_session_order_test.py
python3 tests/libpeer_sctp_config_test.py
python3 tests/libpeer_dtls_read_loop_test.py
python3 tests/datachannel_ppid_test.py
git diff --check
```

Expected: all tests pass.

- [ ] **Step 2: Clean-build the Switch NRO in Docker**

Run:

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    set -e
    export DEVKITPRO=/opt/devkitpro
    export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
    make -f Makefile.switch clean
    make -f Makefile.switch -j$(nproc) \
      NETWORK_DIAG=0 CURL_PROVIDER=wiliwili CURL_VERIFY=0 \
      CURL_VERBOSE=0 CURL_TIMEOUT_MS=30000
  '
```

Expected: `build/switch/LunarNX.nro` is produced successfully.

- [ ] **Step 3: Run the official Xbox path in Ryubing**

Use the saved official token and empty `base_url`. Connect to the discovered Xbox and confirm the log contains:

```text
local ICE candidate: ... 192.168.1.11 ... typ host
add remote ICE candidate: ... 192.168.1.10 9002 typ host
ICE state: 3 (connected)
waitDataChannels ready
```

If the host candidate uses a different current LAN address, validate that address instead. If ICE succeeds but a later stage fails, capture that stage without changing unrelated media code.

- [ ] **Step 4: Package the real-hardware test build**

Copy the NRO and an official `base_url: ""` config into a generated SD-card package under `~/Downloads`. Do not include tokens or logs.

