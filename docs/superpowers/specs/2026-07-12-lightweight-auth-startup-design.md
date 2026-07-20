# Lightweight Auth Startup Design

## Problem

Real Switch hardware closes LunarNX while opening it, before the user can use
the UI. Ryubing loads the same NRO, so simulator startup alone is not sufficient
evidence.

Commit `d7e8b47` previously stabilized hardware startup by keeping the first
screen independent from the full application controller and by removing saved
token auto-navigation from `AuthActivity::createContentView()`. The controller
and streaming components remain lazy today, but saved-session auto-navigation
was later restored inside `createContentView()`. A Switch with `token.json`
therefore creates the HTTP/auth graph and pushes `MainActivity` while the auth
view tree is still being constructed.

## Goal

Make the first rendered screen independent from token state, network state,
the main activity, and all streaming components. Preserve saved sign-in,
sign-out, mock mode, and the existing Xbox device-code flow after an explicit
user action.

## Startup Contract

`AuthActivity::createContentView()` must only:

- Register actions.
- Construct the auth view tree.
- Register button callbacks.
- Return the root view.

Before it returns, it must not:

- Call `controller()`.
- Read `config.json` or `token.json`.
- Construct `HttpClient`, `AuthManager`, or `MainActivity`.
- Push another activity.
- Start network or authentication work.

## User Flow

1. LunarNX always opens on the Xbox Sign-In screen with an enabled `Start`
   button.
2. The first `Start` press lazily creates `StreamController` and reads runtime
   configuration.
3. If mock mode is configured, LunarNX enters `MainActivity` with the existing
   mock controller.
4. Otherwise, LunarNX attempts to load the saved token once.
5. If saved credentials exist, LunarNX enters `MainActivity` without starting
   a network request. Token refresh remains deferred until `Find My Xbox`.
6. If no saved credentials exist, the same button press continues into the
   existing device-code request and polling flow.
7. Sign-out continues to clear credentials and return to a fresh auth activity.

The button label remains `Start` so the flow is identical whether a saved
session exists or a new sign-in is required.

## Implementation Boundary

- Change `resumeSavedSessionIfPresent()` to return whether it navigated to the
  main activity.
- Remove its call from `createContentView()`.
- Invoke it from the `Start` click callback before `beginAuthRequest()`.
- Do not change `MainActivity`, `StreamController` lazy streaming construction,
  Xbox auth payloads, networking, WebRTC, or media behavior.

## Error Handling

- Missing, malformed, or incomplete token data is treated as no saved session;
  the device-code flow starts normally.
- Missing or malformed configuration keeps the existing official Xbox defaults.
- A repeated `Start` press remains protected by the existing auth-request and
  polling atomics.

## Verification

Automated regression checks must prove:

- `createContentView()` does not call `resumeSavedSessionIfPresent()` or
  `controller()`.
- The `Start` callback attempts saved-session resume before device-code auth.
- Saved-session resume only constructs `MainActivity` after `Start`.
- Existing auth UI and stream regressions pass.
- A clean Switch NRO builds in Docker with the official Xbox configuration.
- Ryubing reaches the auth or main screen with and without a saved token.

Real Switch hardware remains the final acceptance test. Success means the NRO
opens reliably before any token, main-page, network, WebRTC, or media work is
performed.
