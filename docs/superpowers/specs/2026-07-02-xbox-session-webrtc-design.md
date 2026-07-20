# Xbox Session/WebRTC Design

## Goal

Align LunarNX's Xbox streaming front half with the shape used by XStreaming and Greenlight while preserving the current xhome behavior. The first implementation phase should make the Xbox protocol, WebRTC negotiation, ICE handling, and data-channel startup easier to reason about and easier to extend to xCloud/cloud gaming later.

## References

Primary references:
- `github_repos/XStreaming/src/xCloud/index.ts`
- `github_repos/XStreaming/src/webrtc/index.ts`
- `github_repos/XStreaming/src/webrtc/Channel/*.ts`
- `github_repos/XStreaming/src/webrtc/Packet/index.ts`
- `github_repos/greenlight/packages/player/src/client/lib/*`
- `github_repos/greenlight/packages/player/src/server/index.ts`
- https://geocld.github.io/2024/11/11/xstreaming-webrtc/
- https://geocld.github.io/2024/07/04/streaming-intro/

The implementation must not copy project-incompatible TypeScript structure directly. It should translate the same responsibilities into small C++ modules that fit LunarNX.

## Scope

In scope:
- Split `StreamController::startStream()` into focused session, signaling, transport, ICE, and channel modules.
- Preserve current xhome/session behavior.
- Add first-class profile types for xhome and xCloud, with xCloud entry points present but not fully enabled.
- Move SDP and ICE exchange into explicit reusable steps.
- Add an ICE candidate processor inspired by XStreaming:
  - Normalize candidate prefixes.
  - Parse `candidates` and `iceCandidates` payloads.
  - Filter invalid UDP candidates containing TCP-only attributes.
  - Preserve `end-of-candidates`.
  - Reorder IPv6 candidates when requested by profile/options.
  - Provide a safe hook for Teredo-derived IPv4 candidates.
- Move data-channel protocol startup into a channel manager:
  - `chat`
  - `control`
  - `input`
  - `message`
- Keep message/control/input startup compatible with the existing behavior:
  - `messageV1` handshake.
  - Wait for `HandshakeAck` or a bounded compatibility timeout.
  - `authorizationRequest`.
  - `gamepadChanged` remove then add.
  - Initial client metadata packet.
- Add regression checks for the new boundaries.

Out of scope for the first implementation phase:
- Full xCloud title catalog UI.
- Full xCloud token acquisition and cloud session launch.
- Persisted stream source setting.
- TURN credential UI.
- New media renderer/audio work.
- Replacing libpeer.

## Architecture

The target ownership is:

`MainActivity -> StreamController -> XboxStreamSession -> WebRtcTransport / XboxSessionClient / XboxChannelManager -> MediaPipeline`

`StreamController` remains the app-facing facade. It owns auth, console lists, stream state callbacks, and lifecycle cancellation. It should no longer contain the detailed protocol choreography.

### Stream Profile

All stream starts use a `StreamProfile`:

```cpp
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
    std::string os_name;
    bool prefer_ipv6 = false;
};
```

`Home` uses `server_id`. `Cloud` will use `title_id` and cloud session metadata later. The first phase may construct only `Home` profiles from the UI, but the API should accept both types.

### XboxSessionClient

`XboxSessionClient` wraps the REST signaling contract:

```cpp
struct ProvisionedSession {
    std::string session_id;
    api::SessionConfig config;
};

class XboxSessionClient {
public:
    explicit XboxSessionClient(std::shared_ptr<api::XboxApiClient> api);
    std::optional<ProvisionedSession> createAndWait(
        const StreamProfile& profile,
        const CancelToken& cancel);
    bool sendSdpOffer(const std::string& session_id,
                      const std::string& offer,
                      const CancelToken& cancel);
    std::string waitSdpAnswer(const std::string& session_id,
                              const CancelToken& cancel);
    bool sendIceCandidates(const std::string& session_id,
                           const std::vector<IceCandidatePayload>& candidates,
                           const CancelToken& cancel);
    std::vector<IceCandidatePayload> waitRemoteIceCandidates(
        const std::string& session_id,
        const StreamProfile& profile,
        const CancelToken& cancel);
    void deleteSessionAsync(const std::string& session_id);
};
```

For `Home`, this delegates to the existing Xbox API endpoints. For `Cloud`, the class should expose the type branch and return a clear unsupported error until cloud endpoints are implemented.

### WebRtcTransport

`WebRtcTransport` wraps `PeerManager` and libpeer-specific operations:

```cpp
class WebRtcTransport {
public:
    bool initialize();
    void setCallbacks(const webrtc::PeerCallbacks& callbacks);
    std::string createOffer();
    bool setRemoteAnswer(const std::string& answer);
    std::vector<webrtc::IceCandidate> gatherLocalCandidates(
        std::chrono::milliseconds stable_window,
        std::chrono::milliseconds timeout,
        const CancelToken& cancel);
    void addRemoteCandidates(const std::vector<IceCandidatePayload>& candidates);
    bool waitDataChannels(std::chrono::milliseconds timeout,
                          const CancelToken& cancel);
    bool isConnected() const;
    void processEvents();
    void disconnect();
};
```

The transport owns only peer state. It does not know about Xbox REST APIs, keepalive, media pipeline, or gamepad semantics.

### IceCandidateProcessor

The processor converts between LunarNX, Xbox API JSON, and libpeer:

```cpp
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
```

XStreaming's Teredo logic is useful but must be isolated. The first phase should add the parser and candidate insertion hook, with conservative behavior by default. If Teredo extraction cannot be implemented safely without extra IPv6 parsing helpers, it should be disabled behind one function and covered by tests for no-op behavior.

### XboxChannelManager

The channel manager owns Xbox data-channel protocol state:

```cpp
class XboxChannelManager {
public:
    explicit XboxChannelManager(WebRtcTransport& transport);
    void reset();
    void handleMessageChannelData(const uint8_t* data, size_t len);
    bool startProtocol(const CancelToken& cancel);
    bool sendInputPacket(const uint8_t* data, size_t len);
    bool sendControlMessage(std::string_view json);
    bool sendMessageHandshake();
    bool requestVideoKeyframe(bool ifr_requested);
};
```

Startup behavior:

1. Send message channel `Handshake`.
2. Wait for `HandshakeAck`, with the current bounded fallback retained for compatibility.
3. Send control `authorizationRequest`.
4. Send `gamepadChanged` removed.
5. After 500 ms, send `gamepadChanged` added.
6. Send initial `ClientMetadata`.
7. Allow periodic input packets.

The exact packet encoding remains in `XInputEncoder`.

### XboxStreamSession

`XboxStreamSession` is the orchestrator:

```cpp
class XboxStreamSession {
public:
    bool start(const StreamProfile& profile,
               const stream::MediaPipelineOptions& media_options,
               const CancelToken& cancel);
    void stop(bool delete_remote_session);
    void tick();
};
```

High-level flow:

1. Create and wait for session provisioning.
2. Initialize `WebRtcTransport`.
3. Create SDP offer.
4. Send offer and wait for answer.
5. Set remote answer.
6. Gather local ICE.
7. Send local ICE.
8. Wait and process remote ICE.
9. Wait for data channels.
10. Initialize media and input.
11. Register media callbacks.
12. Start channel protocol.
13. Enter stream loop for keepalive, token refresh, input, rumble, and reconnect.

Reconnect should reuse the same helpers as initial connect instead of duplicating SDP/ICE logic in the loop.

## xCloud Readiness

The first phase should not ship a working xCloud button. It must make the later xCloud work small and localized:

- `StreamProfile::type == Cloud` is representable.
- `XboxSessionClient` has explicit `Home` and `Cloud` branches.
- Device info construction is profile-based, not hardcoded in one xhome path.
- `title_id`, `region`, and cloud session path can be added without touching `WebRtcTransport`, `IceCandidateProcessor`, or `XboxChannelManager`.
- UI may show a disabled `Stream Source: Xbox / Cloud` control or hide cloud entirely. The code-level profile support is required either way.

## Error Handling

- Every blocking step checks the cancellation token.
- Session creation failures surface clear status messages.
- SDP answer polling has a bounded timeout.
- ICE gathering has both a stable-window timeout and an absolute timeout.
- Empty remote ICE is a recoverable connection failure with diagnostics.
- Data-channel readiness has a bounded timeout.
- Stop deletes the remote session asynchronously when a session id exists.
- Reconnect uses exponential backoff and resets channel protocol state.

## Testing

Required build and regression commands:

```bash
bash scripts/check_stream_regressions.sh
cmake --build build/pc -j4
make -f Makefile.desktop -j4
make -f Makefile.switch -j4
git diff --check
```

Required implementation tests/checks:
- Static regression checks requiring the new session/transport/channel/ICE modules.
- Unit-level tests or small executable checks for `IceCandidateProcessor` parsing:
  - Raw candidate line.
  - `candidate:` line without `a=`.
  - JSON array payload.
  - Object with `iceCandidates`.
  - Object with `candidates`.
  - `a=end-of-candidates`.
  - Invalid UDP candidate containing TCP-only attributes.
  - IPv6 preference ordering.
- Manual xhome validation on Switch remains required for final confidence:
  - Start stream.
  - Stop stream.
  - Reconnect once.
  - Verify input and rumble.
  - Verify media still starts after data channels are ready.

## Acceptance Criteria

- `StreamController::startStream()` is reduced to lifecycle/profile setup and delegation.
- xhome behavior remains functional.
- `SessionType::Cloud` and `StreamProfile` exist and are accepted by the orchestration layer.
- Cloud start returns a clear unsupported result until cloud endpoints are implemented.
- SDP/ICE exchange is not duplicated between initial connect and reconnect.
- Data-channel protocol messages are centralized in `XboxChannelManager`.
- ICE candidate parsing and serialization are centralized in `IceCandidateProcessor`.
- Regression checks prevent collapsing the new boundaries back into `StreamController`.
