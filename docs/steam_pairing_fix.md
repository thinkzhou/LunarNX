# Steam pairing key exchange

The pinned ihslib authorization path sends only an RSA escrow ticket. Current
Steam requires a PIN-masked X25519 public key, validates the shared secret via
the returned encrypted device token, and expects AuthorizationConfirmed.

Ported from mariotaku/ihslib PR 4, commit
f68de5f07e0c27ccea9f47f9f5e38fd92c99919e (LGPL-3.0-or-later).
The dependency changes are reproducible via
scripts/setup_steamlink_dependencies.sh and the tracked pairing patch.
LunarNX retains its timer locking and response endpoint validation.

Live Mac Steam test on 2026-10-31: old authorization returned Failed(7), adding
a timestamp still returned Failed(7), and adding key exchange returned Success.
Host log at 19:18:43 records AuthorizationSuccess followed by receipt of
AuthorizationConfirmed. Subsequent streaming returned DriversNotInstalled;
that is a separate host setup failure, not a pairing failure.

For macOS sanitizer replay with LLVM 23:

    PATH=/opt/homebrew/opt/llvm/bin:$PATH LUNAR_TEST_CXX_INCLUDE=/opt/homebrew/opt/llvm/include/c++/v1 python3 tests/steam_link_hid_sim_test.py

Apple Clang 16's ASan crashes during runtime initialization on this machine.
Pairing has been verified against a real Steam host, not on Switch hardware.
The replay additionally checks a real X25519 peer/device-token proof, rejects
wrong PINs, wrong device IDs and short public keys. LLVM 23 UBSan also reports
existing callback function-type mismatches in BaseWorker and TimerExecute;
the test processes finish successfully, but this is not a clean UBSan run.

The video-first Switch build disables audio in both the initial streaming request
and session negotiation. The same desktop configuration received 1,144 frames
and 2,727,028 bytes in a 20-second real Steam session with zero audio packets.

Host pairing keys and Steam IDs are atomically saved beside the device identity
as `.pairings`, bound to that device ID. Successful authorization saves the host
record; explicit host Unauthorized removes it. Discovery restores the current
endpoint, so an address change does not force re-pairing. The desktop probe saves
per-host keys under its ignored build directory and reuses them on the next run.
These files contain credentials and must not be shared or committed.

Live persistence validation at 19:56 on 2026-10-31: first process paired and
received 261 video frames, saved a host key with permissions 0600, and exited.
A new process launched without `--pair-code` printed `using saved host pairing`,
authenticated and received 492 frames / 6,317,445 bytes in a 10-second run.
Both processes exited successfully. Steam logs show only a streaming request
for the second process (new UDP endpoint), followed by StreamingSuccess, with no
new authorization request. Thus desktop pairing reuse is verified across actual
process restarts; Switch persistence still awaits hardware confirmation.

Saved host addresses are stored atomically in `steamlink_device.json.hosts`.
Manual addresses and authorized hosts discovered on the LAN are remembered.
Entering Steam Link loads the list and probes those IPv4 addresses once per
second alongside broadcast discovery. Explicitly clearing manual discovery
clears the saved list; invalid input does not overwrite it. Up to 64 addresses
are retained. No network discovery is added to application startup.

The address-enabled desktop probe was restarted without an IP or PIN argument:
it logged `probing saved host address=192.168.1.13`, reused its pairing, and
received 537 video frames / 6,467,628 bytes in the 10-second run. The production
Switch client simulation separately validates address restoration and clearing
across client recreation. Broadcast failure itself is not fixed by this fallback.
