#!/usr/bin/env python3
"""Refresh the local PSN token before a UI-free emulator probe."""

import argparse
import base64
import json
import os
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path


CLIENT_ID = "ba495a24-818c-472b-b12d-ff231c1b5745"
CLIENT_SECRET = "mvaiZkRsAsI1IBkY"
REDIRECT_URI = "https://remoteplay.dl.playstation.net/remoteplay/redirect"
TOKEN_URL = "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token"
SCOPE = (
    "psn:clientapp referenceDataService:countryConfig.read "
    "pushNotification:webSocket.desktop.connect "
    "sessionManager:remotePlaySession.system.update"
)


def refresh(path: Path) -> bool:
    data = json.loads(path.read_text())
    now_ms = int(time.time() * 1000)
    expires_at = int(data.get("expires_at_ms", 0) or 0)
    if expires_at > now_ms + 10 * 60 * 1000:
        return False
    refresh_token = data.get("refresh_token", "")
    if not refresh_token:
        raise RuntimeError("PSN token has no refresh_token")
    body = urllib.parse.urlencode({
        "grant_type": "refresh_token",
        "refresh_token": refresh_token,
        "redirect_uri": REDIRECT_URI,
        "scope": SCOPE,
    }).encode()
    basic = base64.b64encode(f"{CLIENT_ID}:{CLIENT_SECRET}".encode()).decode()
    request = urllib.request.Request(
        TOKEN_URL,
        data=body,
        headers={
            "Authorization": f"Basic {basic}",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        updated = json.loads(response.read().decode())
    access_token = updated.get("access_token", "")
    if not access_token:
        raise RuntimeError("PSN refresh response has no access_token")
    data["access_token"] = access_token
    data["refresh_token"] = updated.get("refresh_token") or refresh_token
    data["expires_in"] = int(updated.get("expires_in", 3600) or 3600)
    data["expires_at_ms"] = now_ms + data["expires_in"] * 1000
    handle, temporary = tempfile.mkstemp(prefix="psn_token.", dir=str(path.parent))
    try:
        with os.fdopen(handle, "w") as output:
            json.dump(data, output, separators=(",", ":"))
            output.write("\n")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", type=Path, required=True)
    args = parser.parse_args()
    changed = refresh(args.data_dir / "sdcard/switch/LunarNX/psn_token.json")
    print("PSN token refreshed" if changed else "PSN token still valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
