# Xbox Session/WebRTC Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor LunarNX's Xbox session/WebRTC startup into XStreaming-inspired session, transport, ICE, and channel modules while keeping xhome behavior and device-code login unchanged.

**Architecture:** `StreamController` remains the app-facing facade, but the protocol flow moves into `XboxStreamSession`. `XboxSessionClient` owns REST signaling, `WebRtcTransport` wraps `PeerManager`, `IceCandidateProcessor` owns candidate parsing/serialization, and `XboxChannelManager` owns `chat/control/input/message` startup protocol. `StreamProfile` represents both `Home` and future `Cloud` session types.

**Tech Stack:** C++17 desktop/C++20 Switch, cJSON, existing libpeer `PeerManager`, existing `XboxApiClient`, existing `MediaPipeline`, existing `XInputEncoder`, shell regression checks.

## Global Constraints

- Keep current device-code login flow unchanged.
- Preserve current xhome behavior before enabling xCloud/cloud gaming.
- `SessionType::Cloud` and `StreamProfile` must exist and be accepted by orchestration, but cloud launch returns a clear unsupported result until cloud endpoints are implemented.
- Do not replace libpeer.
- Do not add persistent stream-source settings in this phase.
- Do not change Switch media renderer/audio behavior in this phase.
- Use TDD for pure logic that can be tested locally, especially ICE candidate parsing.
- Every task must keep `bash scripts/check_stream_regressions.sh`, desktop build, and Switch build viable by the end of the task.

---

## File Structure

Create:
- `src/app/stream_profile.h`: `SessionType`, `StreamProfile`, helper defaults.
- `src/app/ice_candidate_processor.h`
- `src/app/ice_candidate_processor.cpp`
- `src/app/web_rtc_transport.h`
- `src/app/web_rtc_transport.cpp`
- `src/app/xbox_channel_manager.h`
- `src/app/xbox_channel_manager.cpp`
- `src/app/xbox_session_client.h`
- `src/app/xbox_session_client.cpp`
- `src/app/xbox_stream_session.h`
- `src/app/xbox_stream_session.cpp`
- `tests/ice_candidate_processor_test.cpp`

Modify:
- `src/app/stream_controller.h`
- `src/app/stream_controller.cpp`
- `src/api/xbox_api_client.h`
- `src/api/xbox_api_client.cpp`
- `CMakeLists.txt`
- `Makefile.desktop`
- `Makefile.switch`
- `scripts/check_stream_regressions.sh`

---

### Task 1: Stream Profile and ICE Test Harness

**Files:**
- Create: `src/app/stream_profile.h`
- Create: `tests/ice_candidate_processor_test.cpp`
- Modify: `Makefile.desktop`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:
  - `enum class lunar::app::SessionType { Home, Cloud };`
  - `struct lunar::app::StreamProfile`
  - `StreamProfile lunar::app::makeHomeStreamProfile(const std::string& server_id, int width, int height)`
  - Make target: `make -f Makefile.desktop stream_tests`

- [ ] **Step 1: Add the failing ICE processor test shell**

Create `tests/ice_candidate_processor_test.cpp` with includes for the not-yet-created processor. This should fail to compile first because `src/app/ice_candidate_processor.h` does not exist.

```cpp
#include "../src/app/ice_candidate_processor.h"
#include "../src/app/stream_profile.h"
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

} // namespace

int main() {
    lunar::app::StreamProfile profile =
        lunar::app::makeHomeStreamProfile("console-1", 1280, 720);
    lunar::app::IceCandidateProcessor processor;

    auto candidates = processor.parseRemotePayload(
        R"({"iceCandidates":[{"candidate":"candidate:1 1 UDP 2130706431 10.0.0.2 9002 typ host","sdpMid":"0","sdpMLineIndex":0}]})",
        profile);

    require(candidates.size() == 1, "expected one parsed candidate");
    require(candidates[0].candidate == "a=candidate:1 1 UDP 2130706431 10.0.0.2 9002 typ host",
            "candidate should be normalized with a= prefix");

    std::cout << "ice candidate processor tests passed\n";
    return 0;
}
```

- [ ] **Step 2: Wire the test target and verify RED**

Add `stream_tests` to `Makefile.desktop`:

```make
STREAM_TEST_OBJS = $(BUILD)/tests/ice_candidate_processor_test.o \
                   $(BUILD)/src/app/ice_candidate_processor.o \
                   $(BUILD)/lib/cJSON.o

.PHONY: all clean run auth_test stream_tests

stream_tests: $(STREAM_TEST_OBJS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $(BUILD)/stream_tests $(STREAM_TEST_OBJS)
	./$(BUILD)/stream_tests
```

Run: `make -f Makefile.desktop stream_tests`

Expected: FAIL at compile time because `src/app/ice_candidate_processor.h` is missing.

- [ ] **Step 3: Add `StreamProfile` minimal implementation**

Create `src/app/stream_profile.h`:

```cpp
#pragma once

#include <string>

namespace lunar::app {

enum class SessionType {
    Home,
    Cloud,
};

struct StreamProfile {
    SessionType type = SessionType::Home;
    std::string server_id;
    std::string title_id;
    int width = 1280;
    int height = 720;
    std::string os_name = "windows";
    bool prefer_ipv6 = false;
};

inline StreamProfile makeHomeStreamProfile(const std::string& server_id,
                                           int width,
                                           int height) {
    StreamProfile profile;
    profile.type = SessionType::Home;
    profile.server_id = server_id;
    profile.width = width;
    profile.height = height;
    profile.os_name = height >= 1080 ? "windows" : "android";
    return profile;
}

inline StreamProfile makeCloudStreamProfile(const std::string& title_id,
                                            int width,
                                            int height) {
    StreamProfile profile;
    profile.type = SessionType::Cloud;
    profile.title_id = title_id;
    profile.width = width;
    profile.height = height;
    profile.os_name = height >= 1080 ? "windows" : "android";
    return profile;
}

} // namespace lunar::app
```

- [ ] **Step 4: Add empty ICE processor interface**

Create `src/app/ice_candidate_processor.h` with the API but no parsing behavior yet:

```cpp
#pragma once

#include "../webrtc/peer_manager.h"
#include "stream_profile.h"
#include <string>
#include <vector>

namespace lunar::app {

struct IceCandidatePayload {
    std::string candidate;
    std::string sdp_mid = "0";
    int sdp_mline_index = 0;
    std::string message_type = "iceCandidate";
};

class IceCandidateProcessor {
public:
    std::vector<IceCandidatePayload> fromLocal(
        const std::vector<webrtc::IceCandidate>& local) const;
    std::vector<IceCandidatePayload> parseRemotePayload(
        const std::string& payload,
        const StreamProfile& profile) const;
    std::string toApiJson(const std::vector<IceCandidatePayload>& candidates) const;
    std::vector<std::string> toLibPeerLines(
        const std::vector<IceCandidatePayload>& candidates) const;
};

} // namespace lunar::app
```

Create `src/app/ice_candidate_processor.cpp` with stub implementations that intentionally fail the test by returning empty results.

- [ ] **Step 5: Verify RED becomes behavioral**

Run: `make -f Makefile.desktop stream_tests`

Expected: executable builds and fails with `FAIL: expected one parsed candidate`.

- [ ] **Step 6: Commit**

```bash
git add src/app/stream_profile.h src/app/ice_candidate_processor.* tests/ice_candidate_processor_test.cpp Makefile.desktop CMakeLists.txt
git commit -m "test: add xbox ice processor harness"
```

### Task 2: IceCandidateProcessor

**Files:**
- Modify: `src/app/ice_candidate_processor.cpp`
- Modify: `tests/ice_candidate_processor_test.cpp`
- Modify: `scripts/check_stream_regressions.sh`

**Interfaces:**
- Consumes:
  - `StreamProfile`
  - `IceCandidatePayload`
- Produces:
  - Correct candidate normalization, JSON parsing, serialization, filtering, ordering.

- [ ] **Step 1: Expand failing tests**

Extend `tests/ice_candidate_processor_test.cpp` to assert:

```cpp
auto raw = processor.parseRemotePayload(
    "candidate:2 1 UDP 1 192.168.1.4 9002 typ host", profile);
require(raw.size() == 1, "raw candidate should parse");
require(raw[0].candidate.rfind("a=candidate:", 0) == 0,
        "raw candidate should gain a= prefix");

auto array = processor.parseRemotePayload(
    R"([{"candidate":"a=candidate:3 1 UDP 1 2001:db8::1 9002 typ host","sdpMid":"0","sdpMLineIndex":0},{"candidate":"a=end-of-candidates","sdpMid":"0","sdpMLineIndex":0}])",
    profile);
require(array.size() == 2, "array payload should parse candidates and end marker");

auto invalid = processor.parseRemotePayload(
    R"({"candidates":[{"candidate":"a=candidate:4 1 UDP 1 10.0.0.3 9 typ host tcptype active","sdpMid":"0","sdpMLineIndex":0}]})",
    profile);
require(invalid.empty(), "invalid UDP candidate with tcptype should be filtered");

lunar::webrtc::IceCandidate local;
local.sdp = "candidate:5 1 UDP 1 10.0.0.4 9002 typ host";
auto api = processor.fromLocal({local});
require(api.size() == 1, "local candidate should convert");
require(api[0].candidate == "candidate:5 1 UDP 1 10.0.0.4 9002 typ host",
        "API candidate should not include a= prefix");

std::string json = processor.toApiJson(api);
require(json.find("\"iceCandidates\"") != std::string::npos,
        "API JSON should use iceCandidates field");

auto lines = processor.toLibPeerLines(array);
require(lines.size() == 1, "libpeer lines should skip end-of-candidates");
require(lines[0].rfind("a=candidate:", 0) == 0,
        "libpeer line should include a= prefix");
```

Run: `make -f Makefile.desktop stream_tests`

Expected: FAIL on at least one missing behavior.

- [ ] **Step 2: Implement parsing helpers**

Implement in `src/app/ice_candidate_processor.cpp`:
- `normalizeForLibPeer()`: trim, add `a=` for `candidate:`, preserve `a=end-of-candidates`.
- `normalizeForApi()`: trim, remove `a=` for candidates, preserve `a=end-of-candidates`.
- `candidateIsInvalid()`: return true if candidate contains both ` UDP ` and `tcptype`.
- `appendCandidateFromJson()`: read object/string candidate, `sdpMid`, `sdpMLineIndex`, `messageType`.
- `parseRemotePayload()`: parse array, object with `iceCandidates`, object with `candidates`, single object, raw line.
- `toApiJson()`: serialize `{"iceCandidates":[...]}`.

- [ ] **Step 3: Verify GREEN**

Run: `make -f Makefile.desktop stream_tests`

Expected: `ice candidate processor tests passed`.

- [ ] **Step 4: Add regression checks**

Add checks to `scripts/check_stream_regressions.sh` requiring:

```bash
src/app/ice_candidate_processor.h
src/app/ice_candidate_processor.cpp
parseRemotePayload
toApiJson
toLibPeerLines
candidateIsInvalid
stream_tests
```

- [ ] **Step 5: Run validation and commit**

```bash
bash scripts/check_stream_regressions.sh
make -f Makefile.desktop stream_tests
git add src/app/ice_candidate_processor.* tests/ice_candidate_processor_test.cpp scripts/check_stream_regressions.sh
git commit -m "feat: add xbox ice candidate processor"
```

### Task 3: WebRtcTransport Wrapper

**Files:**
- Create: `src/app/web_rtc_transport.h`
- Create: `src/app/web_rtc_transport.cpp`
- Modify: `CMakeLists.txt`
- Modify: `Makefile.desktop`
- Modify: `Makefile.switch`
- Modify: `scripts/check_stream_regressions.sh`

**Interfaces:**
- Consumes:
  - `webrtc::PeerManager`
  - `IceCandidatePayload`
- Produces:
  - `class WebRtcTransport`
  - `using CancelCallback = std::function<bool()>;`

- [ ] **Step 1: Add wrapper interface**

Create `src/app/web_rtc_transport.h`:

```cpp
#pragma once

#include "../webrtc/peer_manager.h"
#include "ice_candidate_processor.h"
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace lunar::app {

using CancelCallback = std::function<bool()>;

class WebRtcTransport {
public:
    explicit WebRtcTransport(std::unique_ptr<webrtc::PeerManager> peer);
    ~WebRtcTransport();

    bool initialize();
    void setCallbacks(const webrtc::PeerCallbacks& callbacks);
    std::string createOffer();
    void setRemoteAnswer(const std::string& answer);
    std::vector<webrtc::IceCandidate> gatherLocalCandidates(
        std::chrono::milliseconds stable_window,
        std::chrono::milliseconds timeout,
        const CancelCallback& cancel);
    void addRemoteCandidates(const std::vector<IceCandidatePayload>& candidates);
    bool waitDataChannels(std::chrono::milliseconds timeout,
                          const CancelCallback& cancel);
    bool sendInputData(const uint8_t* data, size_t len);
    bool sendControlData(const uint8_t* data, size_t len);
    bool sendMessageData(const uint8_t* data, size_t len);
    bool isConnected() const;
    void processEvents();
    void disconnect();

private:
    std::unique_ptr<webrtc::PeerManager> peer_;
    IceCandidateProcessor ice_processor_;
};

} // namespace lunar::app
```

- [ ] **Step 2: Implement wrapper by delegation**

Create `src/app/web_rtc_transport.cpp` where every method delegates to `PeerManager`. `gatherLocalCandidates()` loops `processEvents()` until candidates are stable for `stable_window` or total `timeout` expires, mirroring the current `StreamController` behavior.

- [ ] **Step 3: Wire build systems**

Add `src/app/web_rtc_transport.cpp` to:
- `CMakeLists.txt` `LUNARNX_SOURCES`
- `Makefile.desktop` `CXX_SRCS`
- `Makefile.switch` `CXX_SRCS`

- [ ] **Step 4: Verify build**

```bash
make -f Makefile.desktop -j4
make -f Makefile.switch -j4
```

Expected: both builds pass.

- [ ] **Step 5: Add regression checks and commit**

Require `WebRtcTransport`, `gatherLocalCandidates`, and `waitDataChannels` in `scripts/check_stream_regressions.sh`.

```bash
git add src/app/web_rtc_transport.* CMakeLists.txt Makefile.desktop Makefile.switch scripts/check_stream_regressions.sh
git commit -m "refactor: wrap xbox webrtc transport"
```

### Task 4: XboxChannelManager

**Files:**
- Create: `src/app/xbox_channel_manager.h`
- Create: `src/app/xbox_channel_manager.cpp`
- Modify: `CMakeLists.txt`
- Modify: `Makefile.desktop`
- Modify: `Makefile.switch`
- Modify: `scripts/check_stream_regressions.sh`

**Interfaces:**
- Consumes:
  - `WebRtcTransport`
  - `CancelCallback`
- Produces:
  - `class XboxChannelManager`
  - Centralized message/control/input startup methods.

- [ ] **Step 1: Create channel manager API**

```cpp
#pragma once

#include "web_rtc_transport.h"
#include <atomic>
#include <chrono>
#include <string_view>

namespace lunar::app {

class XboxChannelManager {
public:
    explicit XboxChannelManager(WebRtcTransport& transport);

    void reset();
    void handleMessageChannelData(const uint8_t* data, size_t len);
    bool startProtocol(const uint8_t* metadata,
                       size_t metadata_len,
                       const CancelCallback& cancel);
    bool sendInputPacket(const uint8_t* data, size_t len);
    bool sendControlMessage(std::string_view json);
    bool sendMessageHandshake();
    bool requestVideoKeyframe(bool ifr_requested);

private:
    bool waitForHandshake(const CancelCallback& cancel);
    WebRtcTransport& transport_;
    std::atomic<bool> handshake_ready_{false};
};

} // namespace lunar::app
```

- [ ] **Step 2: Implement current protocol behavior**

Implement:
- `sendMessageHandshake()` sends `{"type":"Handshake","version":"messageV1","id":"lunarnx-001","cv":"0"}`.
- `handleMessageChannelData()` sets `handshake_ready_` when payload contains `HandshakeAck`.
- `startProtocol()` sends handshake, waits up to 1500 ms, sends authorization, sends gamepad removed, waits 500 ms, sends gamepad added, sends metadata.
- `requestVideoKeyframe()` sends `{"message":"videoKeyframeRequested","ifrRequested":false}` or true.

- [ ] **Step 3: Wire build systems**

Add `src/app/xbox_channel_manager.cpp` to CMake, desktop Makefile, and Switch Makefile.

- [ ] **Step 4: Add regression checks**

Require `XboxChannelManager`, `sendMessageHandshake`, `authorizationRequest`, `gamepadChanged`, and `videoKeyframeRequested`.

- [ ] **Step 5: Verify and commit**

```bash
bash scripts/check_stream_regressions.sh
make -f Makefile.desktop -j4
make -f Makefile.switch -j4
git add src/app/xbox_channel_manager.* CMakeLists.txt Makefile.desktop Makefile.switch scripts/check_stream_regressions.sh
git commit -m "refactor: centralize xbox data channel startup"
```

### Task 5: XboxSessionClient and XboxStreamSession

**Files:**
- Create: `src/app/xbox_session_client.h`
- Create: `src/app/xbox_session_client.cpp`
- Create: `src/app/xbox_stream_session.h`
- Create: `src/app/xbox_stream_session.cpp`
- Modify: `CMakeLists.txt`
- Modify: `Makefile.desktop`
- Modify: `Makefile.switch`

**Interfaces:**
- Consumes:
  - `StreamProfile`
  - `XboxApiClient`
  - `WebRtcTransport`
  - `XboxChannelManager`
  - `MediaPipeline`
  - `GamepadReader`
  - `XInputEncoder`
  - `RumbleController`
- Produces:
  - `class XboxSessionClient`
  - `class XboxStreamSession`

- [ ] **Step 1: Implement `XboxSessionClient`**

Create a wrapper that delegates Home calls to `XboxApiClient` and returns unsupported for Cloud:

```cpp
enum class SessionStartStatus {
    Ok,
    Cancelled,
    Unsupported,
    Failed,
};

struct ProvisionedSession {
    SessionStartStatus status = SessionStartStatus::Failed;
    std::string session_id;
    api::SessionConfig config;
    std::string error;
};
```

`createAndWait()` should contain the current create/poll/config loop from `StreamController::startStream()`.

- [ ] **Step 2: Implement `XboxStreamSession`**

Move the current high-level startup flow from `StreamController::startStream()` into `XboxStreamSession::start()` using the new helpers. Preserve:
- SDP polling timeout: 20 attempts, 100 ms sleep.
- ICE gather: 800 ms stable window, 5 s absolute timeout.
- Data channel wait: 120 attempts, 16 ms sleep.
- Keepalive interval: `keep_alive_seconds / 2`.
- Token refresh every 15 minutes.
- Reconnect backoff up to 5 retries.

- [ ] **Step 3: Keep media/input ownership injectable**

Constructor should accept references or pointers owned by `StreamController`:

```cpp
XboxStreamSession(XboxSessionClient& session_client,
                  WebRtcTransport& transport,
                  stream::MediaPipeline& media,
                  input::GamepadReader& gamepad,
                  input::XInputEncoder& xinput,
                  input::RumbleController& rumble,
                  stream::PerfStats& perf);
```

- [ ] **Step 4: Wire build systems**

Add new `.cpp` files to CMake, desktop Makefile, and Switch Makefile.

- [ ] **Step 5: Verify compile and commit**

```bash
make -f Makefile.desktop -j4
make -f Makefile.switch -j4
git add src/app/xbox_session_client.* src/app/xbox_stream_session.* CMakeLists.txt Makefile.desktop Makefile.switch
git commit -m "refactor: add xbox stream session orchestration"
```

### Task 6: StreamController Integration and Final Verification

**Files:**
- Modify: `src/app/stream_controller.h`
- Modify: `src/app/stream_controller.cpp`
- Modify: `scripts/check_stream_regressions.sh`

**Interfaces:**
- Consumes:
  - `StreamProfile`
  - `XboxSessionClient`
  - `XboxStreamSession`
  - `WebRtcTransport`
  - `XboxChannelManager`
- Produces:
  - `StreamController::startStream()` reduced to lifecycle/profile setup and delegation.

- [ ] **Step 1: Refactor `StreamController` ownership**

Replace direct `PeerManager` ownership with `WebRtcTransport`. Add `XboxSessionClient`/`XboxStreamSession` as members or create them per start if that keeps lifetime clearer.

- [ ] **Step 2: Preserve public API**

Keep:

```cpp
bool startStream(const std::string& server_id, int width = 1280, int height = 720);
bool startStream(const std::string& server_id, int width, int height,
                 const stream::MediaPipelineOptions& options);
```

Both should create `makeHomeStreamProfile(server_id, width, height)` and delegate.

- [ ] **Step 3: Route callbacks**

Media callbacks still call:
- `media_->decodeVideoPacket`
- `media_->decodeAudioPacket`
- `handleMessageChannelData`
- `rumble_->setRumble`

Channel manager receives message-channel data and owns handshake state.

- [ ] **Step 4: Add static regression checks**

Add checks:
- `StreamController::startStream()` must reference `makeHomeStreamProfile`.
- `StreamController::startStream()` must reference `XboxStreamSession`.
- `StreamController::startStream()` must not contain raw `sendSdpOffer`, `getSdpAnswer`, `sendIceCandidates`, or `getIceCandidates`.
- `SessionType::Cloud` exists.
- `Cloud streaming is not supported yet` or equivalent unsupported string exists.

- [ ] **Step 5: Full validation**

Run:

```bash
make -f Makefile.desktop stream_tests
bash scripts/check_stream_regressions.sh
cmake --build build/pc -j4
make -f Makefile.desktop -j4
make -f Makefile.switch -j4
git diff --check
```

Expected:
- ICE tests pass.
- Regression checks pass.
- PC/desktop/Switch builds pass.

- [ ] **Step 6: Commit**

```bash
git add src/app/stream_controller.* scripts/check_stream_regressions.sh
git commit -m "refactor: delegate xbox stream startup"
```

---

## Self-Review

Spec coverage:
- Stream profile and Cloud readiness: Task 1 and Task 6.
- ICE candidate parsing/serialization: Task 2.
- WebRTC transport wrapper: Task 3.
- Channel manager and protocol startup: Task 4.
- Session REST orchestration and reconnect reuse: Task 5.
- StreamController boundary reduction: Task 6.
- Regression/build verification: all tasks, especially Task 6.

No known placeholders remain. Cloud launch remains intentionally unsupported in phase one, with explicit profile and branch support.
