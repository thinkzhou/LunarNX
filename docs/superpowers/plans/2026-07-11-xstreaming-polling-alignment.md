# XStreaming Signaling Polling Alignment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Poll Xbox SDP and ICE exchange responses with XStreaming's immediate first GET and one-second retry cadence while retaining cancellation and a finite attempt limit.

**Architecture:** `XboxSessionClient` owns one shared signaling interval and attempt limit. Both SDP and ICE perform an immediate GET, parse the first usable response, and use the caller-provided cancellable sleep only between subsequent attempts.

**Tech Stack:** C++20, Xbox REST API, libpeer WebRTC, Python source regressions, Docker/devkitA64, Ryubing Canary.

## Global Constraints

- Use a shared `1000ms` interval for SDP and ICE response polling.
- Keep the maximum at 20 GET attempts.
- Keep the first GET immediate and sleep only between retries.
- Preserve cancellation and current timeout/error messages.
- Do not change payloads, candidate rewriting, WebRTC timeouts, or checklist behavior.
- Build Switch artifacts only in `devkitpro/devkita64:20251117`.

---

### Task 1: Align SDP And ICE Polling

**Files:**
- Create: `tests/xbox_signaling_polling_test.py`
- Modify: `src/app/xbox_session_client.h`
- Modify: `src/app/xbox_session_client.cpp`
- Modify: `src/app/xbox_stream_session.cpp`

**Interfaces:**
- Consumes: `XboxStreamSession::sleepUnlessCancelled()` through `SleepCallback`.
- Produces: `XboxSessionClient::getIceCandidates(session_id, profile, cancel, sleep)`.

- [x] **Step 1: Write the failing source regression**

Create a Python test that requires:

```python
source = Path("src/app/xbox_session_client.cpp").read_text()
header = Path("src/app/xbox_session_client.h").read_text()
session = Path("src/app/xbox_stream_session.cpp").read_text()

require("kSignalingPollInterval{1000}" in source,
        "signaling polling must use XStreaming's 1000ms cadence")
require(source.count("sleep(kSignalingPollInterval)") == 2,
        "SDP and ICE retries must both use cancellable signaling sleep")
require("std::this_thread::sleep_for" not in source,
        "ICE polling must not bypass cancellation")
require("const SleepCallback& sleep" in header,
        "ICE polling must accept the cancellable sleep callback")
require("getIceCandidates(session_id, profile, cancel, sleep)" in session,
        "the stream session must pass its cancellable sleep callback")
```

- [x] **Step 2: Run the test and verify RED**

Run:

```sh
python3 tests/xbox_signaling_polling_test.py
```

Expected: failure reporting the missing `1000ms` shared interval.

- [x] **Step 3: Implement the shared polling contract**

In `src/app/xbox_session_client.cpp`, add:

```cpp
namespace {
constexpr std::chrono::milliseconds kSignalingPollInterval{1000};
constexpr int kSignalingPollAttempts = 20;
}
```

For both SDP and ICE, perform the GET before sleeping. After an empty or
unparseable result, return on cancellation; otherwise call
`sleep(kSignalingPollInterval)` only when another attempt remains. Remove the
direct `std::this_thread::sleep_for` call and its unused `<thread>` include.

Change the ICE method declaration and definition to:

```cpp
std::vector<IceCandidatePayload> getIceCandidates(
    const std::string& session_id,
    const StreamProfile& profile,
    const CancelCallback& cancel,
    const SleepCallback& sleep);
```

Pass `sleep` from `XboxStreamSession::negotiateWebRtc()`.

- [x] **Step 4: Run focused tests and verify GREEN**

Run:

```sh
python3 tests/xbox_signaling_polling_test.py
python3 tests/xbox_stream_session_order_test.py
python3 tests/libpeer_ice_checklist_test.py
python3 tests/libpeer_ipv6_candidate_test.py
python3 tests/xstreaming_ice_server_test.py
make -f Makefile.desktop stream_tests
git diff --check
```

Expected: all tests pass and whitespace check produces no output.

- [x] **Step 5: Build and run simulator regressions**

Build the NRO in Docker using the repository's documented Switch command.
Run one Ryubing `xbox-teredo` mock stream and require `ICE connected`,
`waitDataChannels ready`, and a media performance line without SRTP or decode
errors. Restore official `base_url`, then run one official attempt and record
the returned candidate set and whether any STUN packet was received.

- [x] **Step 6: Commit the completed alignment**

Stage the polling implementation, regression, and the already verified
XStreaming ICE/trickle changes. Exclude tokens, logs, build output, simulator
configuration, and dependency build directories. Commit with:

```sh
git commit -m "fix: align Xbox ICE negotiation with XStreaming"
```
