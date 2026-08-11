# PlayStation Page and Launch Flow Design

## Goal

Make PlayStation Remote Play understandable and correct on Nintendo Switch by
separating account state, console identity, LAN pairing, and PSN remote launch.
The page must only offer actions that the current route can perform, while both
LAN and remote sessions continue to reuse the shared LunarNX media and stream
UI boundary.

## Verified Baseline

The following behavior has been exercised in Ryubing:

- PSN OAuth login and persisted refresh-token restoration.
- Expired or rejected access-token recovery.
- A device-list HTTP 401 forces one refresh and one retry.
- The PS5 device endpoint returns both Remote-Play-enabled and disabled devices.
- The current account returned `PS5-231` with Remote Play enabled and `PS5-200`
  with Remote Play disabled.

These results prove the account and directory path. They do not prove LAN
pairing, host identity merging, wake, NAT traversal, or media startup.

## Protocol Workflows

### LAN Remote Play

```text
Search local network
  -> discover PS4/PS5 and canonical server MAC
  -> if not paired, open Pair flow
  -> user opens Link/Add Device on the console
  -> user enters the temporary 8-digit Remote Play PIN
  -> Chiaki registration returns server MAC, nickname, RP-RegistKey, and RP-Key
  -> persist credentials by server MAC
  -> Ready: connect locally
  -> Standby: wake, wait for the same MAC to become Ready, then connect
  -> Chiaki session -> H.264/Opus callbacks -> shared MediaPipeline
```

The 8-digit Remote Play PIN is temporary pairing input. It must never be stored
as the console login PIN.

### PSN Remote Play

```text
Restore or refresh PSN OAuth session
  -> list PS5 devices and retain their 64-character DUID
  -> user selects an enabled device
  -> create PSN Remote Play session
  -> create the control offer
  -> start/wake the selected console
  -> punch the control UDP channel
  -> hand the hole-punch session to ChiakiSession
  -> Chiaki creates control RUDP
  -> Chiaki performs PSN registration with pin=0 and the PSN account ID
  -> console supplies the RP-RegistKey and RP-Key required for this session
  -> Chiaki performs session request and control startup
  -> satisfy optional console login-PIN request
  -> Chiaki creates and punches the data channel
  -> Senkusha and Takion start the media connection
  -> H.264/Opus callbacks -> shared MediaPipeline
```

PSN remote launch does not require a previously saved LAN pairing key. It still
uses registration credentials internally, but chiaki-ng obtains them during the
remote session over control RUDP. LunarNX must not punch the data channel before
`chiaki_session_start()` or manage that socket outside ChiakiSession.

## PIN Terminology

| PIN | Length | Source | Lifetime | Purpose |
|---|---:|---|---|---|
| Remote Play pairing PIN | 8 digits | Link/Add Device screen | One pairing attempt | Creates persistent LAN credentials |
| Console login PIN | Usually 4 digits | Console user profile | Per session or optionally remembered | Satisfies `CHIAKI_EVENT_LOGIN_PIN_REQUEST` |

A console login-PIN request is not a pairing failure and must not tell the user
to re-register the console.

## Console Identity

Do not overload one string with unrelated identifiers.

```cpp
struct PsConsole {
    std::optional<std::string> server_mac; // 12 lowercase hex characters
    std::optional<std::string> psn_duid;   // 64 lowercase hex characters
    std::string nickname;
    std::optional<PsLocalEndpoint> local;
    std::optional<PsRemoteEndpoint> remote;
    std::optional<RegisteredCredential> credentials;
};
```

Rules:

1. Registration-result server MAC is the primary identity for persisted local
   credentials.
2. PSN DUID remains textual in models and persistence, and is decoded to 32 bytes
   only at the hole-punch API boundary.
3. Persist an exact server-MAC-to-DUID association once known.
4. A unique nickname match may propose the first association, but nickname is
   never the durable primary key.
5. Ambiguous nicknames remain separate until explicitly resolved.
6. Binary DUID prefix searches are invalid and must not be used.

## Page Information Architecture

The PlayStation page follows the same source-tab model as the Xbox page:

```text
PlayStation Remote Play

[ Local Remote Play ] [ Remote via PSN ]

Account
  PlayStation Network status
  Sign In / Refresh / Switch Account

Local Remote Play tab
  fresh LAN discovery results
  Search LAN
  Pair PS4 by IP / Pair PS5 by IP

Remote via PSN tab
  PS5 devices returned by the PSN account
  Refresh PSN
```

The tabs deliberately separate local Pair/Wake behavior from remote PSN
availability. A console can appear in both views when it has both routes; the
selected tab fixes the requested route instead of silently switching between
local and remote semantics.

## Account States

| State | Presentation | Primary action |
|---|---|---|
| Signed out | Local pairing and LAN streaming remain available | Sign In |
| Restoring | Restoring saved PSN session | Disabled while running |
| Signed in | Remote PS5 discovery is available | Refresh PSN |
| Session expired | Saved session cannot be refreshed | Sign In Again |
| Device lookup failed | Account remains signed in | Retry |

`hasStoredSession()` alone is not sufficient to claim that the account is signed
in. The page must distinguish stored credentials from a usable/restorable PSN
session.

## Console Card Actions

| Credentials | Fresh LAN state | PSN state | Primary action |
|---|---|---|---|
| Missing | Ready | Any | Pair |
| Missing | Standby | Any | Pair after waking/opening Link Device |
| Missing | None | Remote enabled | Connect remotely |
| Missing | None | Remote disabled | No action; explain how to enable Remote Play |
| Present | Ready | Any | Connect |
| Present | Standby | Any | Wake & Connect |
| Present | None | Remote enabled and signed in | Connect |
| Present | None | No usable remote route | Unavailable |

A PSN-only enabled PS5 must not show the traditional LAN Pair action. Normal PSN
remote streaming obtains session credentials dynamically.

When exact identity mapping shows LAN and PSN represent the same console, render
one card. A fresh LAN route wins; if the LAN route fails before streaming, retry
through PSN when available.

## LAN Search

Search must be active and bounded rather than a permanent receive-only listener.

```text
Idle
  -> Searching
  -> incremental host updates
  -> Complete(count)
  -> Failed(error)
```

Requirements:

- Send search packets on a bounded cadence or use `ChiakiDiscoveryService`.
- Timestamp each result and expire stale endpoints.
- Every search action creates a new generation.
- Complete with an explicit zero-result state.
- Stop and invalidate callbacks when the page exits.
- Never invoke UI callbacks while holding the discovery mutex.

## Pair Flow

```text
Pair PlayStation
  1. Open Settings > Remote Play > Link/Add Device on the console.
  2. Confirm the console address.
  3. Enter the 8-digit Remote Play pairing PIN.
  4. Pair and save credentials by the returned server MAC.
```

The form preserves leading zeroes, disables inputs while pairing, supports
cancellation, and returns to a refreshed console card on success. User-facing
copy uses Pair; internal Chiaki types may continue to use Registration.

## Route and Launch State

The card exposes one goal-oriented action. Route selection remains internal.

```text
Idle
  -> ResolvingRoute
  -> LocalReady
       -> ConnectingLocal
  -> LocalNeedsWake
       -> Waking
       -> WaitingForConsole
       -> ConnectingLocal
  -> Remote
       -> RefreshingPsnSession
       -> CreatingRemoteSession
       -> StartingConsole
       -> PunchingControl
       -> RegisteringRemoteSession
       -> StartingControl
       -> WaitingForConsoleLoginPin
       -> PunchingData
  -> NegotiatingStream
  -> WaitingForVideo
  -> Ready
```

A stale LAN endpoint is not a route. Standby is not equivalent to Ready. Wake &
Connect waits for a fresh observation of the same MAC and uses the newly observed
address.

## Loading and Cancellation

Reuse and generalize `StreamLoadingActivity`; do not add a PlayStation-specific
copy. Its launch adapter remains protocol-neutral and supports:

- start;
- cancel;
- phase/detail updates;
- terminal error separate from progress;
- media-ready notification;
- optional console login-PIN request and submission;
- resulting `IStreamRuntime`.

The loading page remains visible until the session is connected and the first
usable video sample has reached the media pipeline. It must not enter
`StreamView` merely because `chiaki_session_start()` returned successfully.

Cancellation must interrupt wake waiting, token refresh, PSN session creation,
hole punching, session startup, login-PIN waiting, and first-video waiting.

## Persistence

### `psn_token.json`

Store separately from host credentials:

- access token;
- refresh token;
- expiry;
- PSN account ID;
- OAuth client DUID.

### `ps_credentials.json`

Use a versioned, atomic schema keyed by canonical server MAC:

- target;
- server MAC;
- nickname;
- optional PSN DUID mapping;
- last known address;
- RP-RegistKey;
- RP-Key and key type;
- optional console login PIN.

Do not persist:

- temporary 8-digit pairing PIN;
- discovery freshness timestamps;
- PSN OAuth tokens;
- AP credentials that LunarNX does not use.

Existing unversioned records may migrate through a valid non-zero `server_mac`.
The old `console_pin` is discarded because current code populated it from the
pairing PIN and it cannot be trusted as a console login PIN.

Both token and credential files use temporary-file write, flush/close, and rename
to avoid truncating the active file.

## Error Presentation

Distinguish:

- PSN session expired;
- Remote Play disabled on the console;
- console offline or unreachable;
- LAN wake timeout;
- NAT traversal failure;
- pairing failure;
- console login PIN incorrect;
- media startup failure.

Progress text never overwrites the terminal error field.

## Implementation Boundaries

Preserve:

- PSN OAuth and 401 refresh/retry behavior;
- LAN discovery adapter where its callback mapping is valid;
- Chiaki registration-result field copying;
- `PsMediaBridge`, shared `MediaPipeline`, `IStreamRuntime`, and `StreamView`;
- Xbox session, API, WebRTC, and UI behavior.

Replace:

- connector-side data punching;
- duplicated hole-punch finalization;
- binary DUID model values and prefix matching;
- empty `host_id` credential persistence;
- pairing-PIN persistence;
- receive-only/unbounded LAN search;
- wake-only card behavior;
- direct `PsActivity` to `StreamView` startup;
- login-PIN-as-error handling.

## Acceptance Criteria

- PSN device refresh continues to return enabled and disabled PS5 devices.
- An enabled PSN-only PS5 exposes Connect without traditional Pair.
- Remote logs show control hole before session start and data hole inside the
  Chiaki session.
- LAN pairing survives restart and is keyed by server MAC.
- The temporary pairing PIN is absent from persisted JSON.
- The same mapped console renders once when both LAN and PSN are present.
- Search LAN reaches Complete or Failed and stale endpoints do not win routing.
- Standby uses Wake & Connect.
- Console login PIN can be entered without re-pairing.
- Loading supports progress and cancellation through first video.
- Xbox Remote Play and xCloud regressions remain green.
- Switch NRO BSS remains below 32 MiB.

## Out of Scope

- PS5 HEVC/HDR.
- Advanced DualSense features.
- General relay service for NAT types that Sony/Chiaki hole punching cannot
  traverse.
- PS4 PSN device enumeration beyond chiaki-ng's Main PS4 limitation.
- Encrypting secrets at rest before a suitable Switch keystore is selected.
