# Chiaki Switch ABI Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the Chiaki public-ABI mismatch that crashes PSN Remote at `chiaki_session_init()` and make future mismatches fail during the build.

**Architecture:** Build the Chiaki archive, public headers, and generated headers as one Docker-produced SDK. Export a small ABI fingerprint from the archive and compare it against values compiled with LunarNX consumer flags before linking the NRO.

**Tech Stack:** C/C++, GNU Make, CMake, devkitA64 Docker, Python regression tests

---

### Task 1: Add a failing ABI regression

**Files:**
- Create: `tests/chiaki_switch_abi_test.py`
- Create: `tools/chiaki_switch/abi_probe.c`

- [ ] Compile a consumer probe with the Switch headers and LunarNX ABI defines.
- [ ] Extract the matching constants from `libchiaki.a`.
- [ ] Verify the test fails against the current mismatch with a size/offset error.

### Task 2: Produce one coherent Chiaki SDK

**Files:**
- Create: `tools/chiaki_switch/build_in_docker.sh`
- Create: `tools/chiaki_switch/README.md`
- Modify: `github_repos/chiaki-ng/CMakeLists.txt`
- Modify: `github_repos/chiaki-ng/lib/CMakeLists.txt`
- Modify: `Makefile.switch`
- Modify: `Makefile.switch.psprobe`

- [ ] Export library ABI fingerprint values from a source compiled into Chiaki.
- [ ] Build Chiaki from the local checkout using the pinned devkitA64 image.
- [ ] Install archive and headers atomically into `lib/switch`.
- [ ] Propagate all public ABI definitions to LunarNX consumers.
- [ ] Run the ABI regression and verify it passes.

### Task 3: Align alternate build paths and dependency tests

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `tests/ps_switch_runtime_dependencies_test.py`

- [ ] Add a failing assertion that Switch CMake must not compile holepunch stubs.
- [ ] Remove the obsolete stub source and require real PSN Remote dependencies.
- [ ] Make the STUN assertion match the installed Switch Chiaki implementation.
- [ ] Run all focused PS tests.

### Task 4: Full Switch verification

**Files:**
- No production file changes expected.

- [ ] Clean-build the Switch NRO in `devkitpro/devkita64:20251117`.
- [ ] Run `tests/switch_nro_bss_test.py`.
- [ ] Run focused WebRTC/session tests and `git diff --check`.
- [ ] Record simulator limitations and the required real-hardware follow-up.

