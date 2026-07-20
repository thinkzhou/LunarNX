# Lightweight Auth Startup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ensure LunarNX renders its auth screen on real Switch hardware before constructing the application controller, reading saved tokens, or navigating to the main activity.

**Architecture:** Keep `AuthActivity::createContentView()` limited to view construction. Move saved-session and mock-mode restoration behind the existing `Start` callback, returning a boolean so the callback can either stop after navigation or continue into the device-code flow.

**Tech Stack:** C++20, Borealis, libnx, Python source regression, Docker/devkitA64, Ryubing Canary.

## Global Constraints

- Switch builds use `devkitpro/devkita64:20251117` only.
- Do not modify Xbox payloads, networking, WebRTC, media, or `MainActivity`.
- Preserve saved tokens, sign-out, mock mode, and device-code authentication.
- `AuthActivity::createContentView()` must not call `controller()`, read saved state, or push `MainActivity`.
- Real Switch hardware is the final acceptance target.

---

### Task 1: Defer Saved Session Restoration Until Start

**Files:**
- Create: `tests/auth_startup_laziness_test.py`
- Modify: `src/ui/auth_activity.h`
- Modify: `src/ui/auth_activity.cpp`
- Test: `tests/auth_startup_laziness_test.py`

**Interfaces:**
- Consumes: existing `AuthActivity::controller()` and `AuthActivity::beginAuthRequest()`.
- Produces: `bool AuthActivity::resumeSavedSessionIfPresent()`; `true` means navigation to `MainActivity` was scheduled.

- [x] **Step 1: Write the failing startup-boundary regression**

Create `tests/auth_startup_laziness_test.py` that extracts the bodies of
`createContentView()` and the Start callback and requires:

```python
require("resumeSavedSessionIfPresent();" not in create_body,
        "auth view construction must not restore a saved session")
require("controller()" not in create_body,
        "auth view construction must not create the application controller")
require("if (resumeSavedSessionIfPresent())" in start_callback,
        "Start must try saved-session restoration before device-code auth")
require(start_callback.index("resumeSavedSessionIfPresent") <
        start_callback.index("beginAuthRequest"),
        "saved-session restoration must precede device-code auth")
require("bool AuthActivity::resumeSavedSessionIfPresent()" in source,
        "saved-session restoration must report whether it navigated")
```

- [x] **Step 2: Run the regression and verify RED**

Run:

```sh
python3 tests/auth_startup_laziness_test.py
```

Expected: failure stating that auth view construction still restores a saved
session.

- [x] **Step 3: Implement lazy restoration**

Change the Start callback in `createContentView()` to:

```cpp
start_btn_->registerClickAction([this](brls::View*) -> bool {
    if (resumeSavedSessionIfPresent()) {
        return true;
    }
    beginAuthRequest();
    return true;
});
```

Remove the unconditional `resumeSavedSessionIfPresent();` call before returning
the root view. Change the method declaration and definition to return `bool`.
Return `false` when it was already attempted or no saved credentials exist;
return `true` after scheduling mock or saved-session navigation.

- [x] **Step 4: Run focused verification and verify GREEN**

Run:

```sh
python3 tests/auth_startup_laziness_test.py
make -f Makefile.desktop auth_ui_tests
bash scripts/check_stream_regressions.sh
git diff --check
```

Expected: all commands exit zero.

- [x] **Step 5: Build and smoke-test the Switch NRO**

Perform a clean Docker build with:

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

Run Ryubing once with `token.json` present and once with it temporarily moved
aside. In both cases, require the app log to reach `mainLoop begin` without
`MainActivity create` before a Start press. Restore the token and remove test
logs afterward.

- [x] **Step 6: Commit the implementation**

Stage only the source, regression, and plan. Exclude tokens, logs, NROs, build
output, and simulator data. Commit with:

```sh
git commit -m "fix: defer saved auth startup"
```
