#!/usr/bin/env python3
import argparse
import asyncio
import base64
import json
import os
import re
import ssl
import subprocess
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlsplit

import requests
from websockets.asyncio.client import connect

FQDN_URL = (
    "https://mobile-pushcl.np.communication.playstation.net/np/serveraddr"
    "?version=2.1&fields=keepAliveStatus&keepAliveStatusType=3"
)
SESSION_URL = "https://web.np.playstation.com/api/sessionManager/v1/remotePlaySessions"
TOKEN_URL = "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token"
PSN_CLIENT_ID = "ba495a24-818c-472b-b12d-ff231c1b5745"
PSN_CLIENT_SECRET = "mvaiZkRsAsI1IBkY"
PSN_REDIRECT_URI = "https://remoteplay.dl.playstation.net/remoteplay/redirect"
PSN_SCOPE = (
    "psn:clientapp referenceDataService:countryConfig.read "
    "pushNotification:webSocket.desktop.connect "
    "sessionManager:remotePlaySession.system.update"
)

SENSITIVE_JSON_FIELDS = (
    "accountId", "customData1", "deviceUniqueId", "duid", "onlineId",
    "sessionId",
)

FULL_FLOW_PHASES = (
    ("list_devices", "ok"),
    ("select_device", "ok"),
    ("session_create", "ok"),
    ("control_offer", "ok"),
    ("console_start", "ok"),
    ("control_hole_punch", "ok"),
    ("stream_session_start", "ok"),
    ("data_hole_punch", "started"),
    ("data_hole_punch", "ok"),
    ("session_connected", "ok"),
    ("first_opus_packet", "ok"),
    ("first_h264_sample", "ok"),
    ("stream_stop", "ok"),
    ("session_quit", "ok"),
    ("stream_join", "ok"),
    ("psn_session_cleanup", "ok"),
    ("media_summary", "ok"),
)


def validate_full_flow(phases, exit_code, relay_expected=False):
    errors = []
    cursor = 0
    if relay_expected and not any(
            phase.get("phase") == "relay_config"
            and phase.get("status") == "ok" for phase in phases):
        errors.append("relay_config:ok is missing")
    for expected_phase, expected_status in FULL_FLOW_PHASES:
        found = False
        while cursor < len(phases):
            phase = phases[cursor]
            cursor += 1
            if (phase.get("phase") == expected_phase
                    and phase.get("status") == expected_status):
                found = True
                break
        if not found:
            errors.append(f"missing ordered phase {expected_phase}:{expected_status}")
            break
    media = next((phase for phase in reversed(phases)
                  if phase.get("phase") == "media_summary"), {})
    for field in ("connected", "registered", "data_hole", "clean_stop"):
        if media.get(field) is not True:
            errors.append(f"media_summary.{field} is not true")
    for field in ("video_frames", "audio_frames"):
        if not isinstance(media.get(field), int) or media[field] <= 0:
            errors.append(f"media_summary.{field} is not positive")
    if media.get("frames_lost") != 0:
        errors.append("media_summary.frames_lost is not zero")
    if exit_code != 0:
        errors.append(f"native probe exit code is {exit_code}")
    return {"passed": not errors, "errors": errors}


def redact_text(value, secrets=()):
    for secret in secrets:
        if secret:
            value = value.replace(secret, "<redacted>")
    for field in SENSITIVE_JSON_FIELDS:
        value = re.sub(
            rf'(\"{field}\"\s*:\s*\")[^\"]*(\")',
            rf'\1<redacted>\2', value, flags=re.IGNORECASE)
    value = re.sub(
        r"\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b",
        "<uuid:redacted>", value, flags=re.IGNORECASE)
    value = re.sub(r"\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b", "<ipv4:redacted>", value)
    value = re.sub(r"\b[0-9a-f]{32,}\b", "<hex:redacted>", value,
                   flags=re.IGNORECASE)
    if re.search(r"(?:^|\s)(?:[0-9a-f]{2}\s+){8,}", value,
                 flags=re.IGNORECASE):
        return "<binary-hexdump:redacted>"
    return value


def redact_headers(headers):
    return {
        key: "Bearer <redacted>" if key.lower() == "authorization" else value
        for key, value in headers.items()
    }


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def refresh_token_if_needed(token_path, timeout):
    token_data = json.loads(token_path.read_text())
    now_ms = int(time.time() * 1000)
    if (token_data.get("access_token") and
            token_data.get("expires_at_ms", 0) > now_ms + 60_000):
        return token_data, False
    refresh_token = token_data.get("refresh_token", "")
    if not refresh_token:
        raise SystemExit("PSN access token expired and no refresh_token is available")

    credentials = f"{PSN_CLIENT_ID}:{PSN_CLIENT_SECRET}".encode()
    authorization = "Basic " + base64.b64encode(credentials).decode()
    response = requests.post(
        TOKEN_URL,
        headers={
            "Authorization": authorization,
            "Content-Type": "application/x-www-form-urlencoded",
        },
        data={
            "grant_type": "refresh_token",
            "refresh_token": refresh_token,
            "redirect_uri": PSN_REDIRECT_URI,
            "scope": PSN_SCOPE,
        },
        timeout=timeout,
    )
    response.raise_for_status()
    refreshed = response.json()
    access_token = refreshed.get("access_token", "")
    if not access_token:
        raise RuntimeError("PSN refresh response has no access_token")
    token_data["access_token"] = access_token
    token_data["refresh_token"] = refreshed.get("refresh_token") or refresh_token
    expires_in = int(refreshed.get("expires_in") or 3600)
    token_data["expires_in"] = expires_in
    token_data["expires_at_ms"] = now_ms + expires_in * 1000

    temp_path = token_path.with_name(token_path.name + ".tmp")
    temp_path.write_text(json.dumps(token_data, indent=2) + "\n")
    os.chmod(temp_path, 0o600)
    os.replace(temp_path, token_path)
    return token_data, True


def response_record(response, started_at):
    try:
        body = response.json()
    except ValueError:
        body = response.text
    return {
        "status": response.status_code,
        "headers": dict(response.headers),
        "body": body,
        "elapsed_ms": round((time.monotonic() - started_at) * 1000, 1),
    }


def websocket_response_record(websocket, started_at):
    response = websocket.response
    return {
        "status": response.status_code,
        "headers": dict(response.headers.raw_items()),
        "elapsed_ms": round((time.monotonic() - started_at) * 1000, 1),
    }


async def run_probe(token_path, output_dir, insecure, timeout, listen_seconds):
    token = json.loads(token_path.read_text()).get("access_token", "")
    if not token:
        raise SystemExit("Token file has no access_token")

    output_dir.mkdir(parents=True, exist_ok=False)
    authorization = f"Bearer {token}"
    common_headers = {"Authorization": authorization}
    verify = not insecure

    metadata = {
        "started_at": datetime.now(timezone.utc).isoformat(),
        "token_path": str(token_path),
        "tls_verification": verify,
        "timeout_seconds": timeout,
        "listen_seconds": listen_seconds,
    }
    write_json(output_dir / "00_metadata.json", metadata)

    write_json(output_dir / "01_fqdn_request.json", {
        "method": "GET",
        "url": FQDN_URL,
        "headers": redact_headers(common_headers),
    })
    started_at = time.monotonic()
    fqdn_response = requests.get(
        FQDN_URL, headers=common_headers, timeout=timeout, verify=verify)
    write_json(output_dir / "02_fqdn_response.json",
               response_record(fqdn_response, started_at))
    fqdn_response.raise_for_status()
    fqdn = fqdn_response.json().get("fqdn", "")
    if not fqdn:
        raise RuntimeError("FQDN response contains no fqdn")

    push_context_id = str(uuid.uuid4())
    ws_url = f"wss://{fqdn}/np/pushNotification"
    ws_headers = {
        "Authorization": authorization,
        "X-PSN-APP-TYPE": "REMOTE_PLAY",
        "X-PSN-APP-VER": "RemotePlay/1.0",
        "X-PSN-KEEP-ALIVE-STATUS-TYPE": "3",
        "X-PSN-OS-VER": "Windows/10.0",
        "X-PSN-PROTOCOL-VERSION": "2.1",
        "X-PSN-RECONNECTION": "false",
    }
    write_json(output_dir / "03_websocket_request.json", {
        "url": ws_url,
        "subprotocols": ["np-pushpacket"],
        "headers": redact_headers(ws_headers),
    })

    ssl_context = ssl.create_default_context()
    if insecure:
        ssl_context.check_hostname = False
        ssl_context.verify_mode = ssl.CERT_NONE

    started_at = time.monotonic()
    async with connect(
        ws_url,
        subprotocols=["np-pushpacket"],
        additional_headers=ws_headers,
        user_agent_header="WebSocket++/0.8.2",
        ssl=ssl_context,
        proxy=None,
        open_timeout=timeout,
        ping_interval=5,
        ping_timeout=5,
    ) as websocket:
        write_json(output_dir / "04_websocket_response.json",
                   websocket_response_record(websocket, started_at))

        session_body = {
            "remotePlaySessions": [{
                "members": [{
                    "accountId": "me",
                    "deviceUniqueId": "me",
                    "platform": "me",
                    "pushContexts": [{"pushContextId": push_context_id}],
                }],
            }],
        }
        session_headers = {
            "Authorization": authorization,
            "Content-Type": "application/json; charset=utf-8",
        }
        write_json(output_dir / "05_session_request.json", {
            "method": "POST",
            "url": SESSION_URL,
            "headers": redact_headers(session_headers),
            "body": session_body,
        })

        started_at = time.monotonic()
        try:
            session_response = await asyncio.to_thread(
                requests.post,
                SESSION_URL,
                headers=session_headers,
                json=session_body,
                timeout=timeout,
                verify=verify,
            )
            write_json(output_dir / "06_session_response.json",
                       response_record(session_response, started_at))
        except Exception as exc:
            write_json(output_dir / "06_session_response.json", {
                "error": f"{type(exc).__name__}: {exc}",
                "elapsed_ms": round((time.monotonic() - started_at) * 1000, 1),
            })
            raise

        notifications = []
        deadline = asyncio.get_running_loop().time() + listen_seconds
        while True:
            remaining = deadline - asyncio.get_running_loop().time()
            if remaining <= 0:
                break
            try:
                message = await asyncio.wait_for(websocket.recv(), remaining)
            except TimeoutError:
                break
            if isinstance(message, bytes):
                try:
                    decoded = message.decode("utf-8")
                    payload = json.loads(decoded)
                except (UnicodeDecodeError, json.JSONDecodeError):
                    payload = {"binary_hex": message.hex()}
            else:
                try:
                    payload = json.loads(message)
                except json.JSONDecodeError:
                    payload = message
            notifications.append({
                "received_at": datetime.now(timezone.utc).isoformat(),
                "payload": payload,
            })
        write_json(output_dir / "07_websocket_notifications.json", notifications)

    summary = {
        "output_dir": str(output_dir),
        "fqdn": fqdn,
        "session_status": session_response.status_code,
        "notifications": len(notifications),
    }
    write_json(output_dir / "08_summary.json", summary)
    return summary


def run_full_probe(token_path, output_dir, native_binary, device, media_seconds,
                   login_pin, timeout):
    token_data, token_refreshed = refresh_token_if_needed(token_path, timeout)
    token = token_data.get("access_token", "")
    account_id = token_data.get("account_id", "")
    refresh_token = token_data.get("refresh_token", "")
    if not token or not account_id:
        raise SystemExit("Token file has no access_token or account_id")
    if not native_binary.is_file() or not os.access(native_binary, os.X_OK):
        raise SystemExit(f"Native probe is not executable: {native_binary}")

    output_dir.mkdir(parents=True, exist_ok=False)
    relay_expected = bool(os.environ.get("LUNARNX_PS_RELAY_HOST"))
    write_json(output_dir / "00_metadata.json", {
        "started_at": datetime.now(timezone.utc).isoformat(),
        "token_path": str(token_path),
        "device": device,
        "media_seconds": media_seconds,
        "native_binary": str(native_binary),
        "login_pin_supplied": bool(login_pin),
        "token_refreshed": token_refreshed,
        "trace_redacted": True,
        "media_payloads_saved": False,
        "relay_enabled": relay_expected,
    })

    env = os.environ.copy()
    env["LUNARNX_PSN_ACCESS_TOKEN"] = token
    env["LUNARNX_PSN_ACCOUNT_ID"] = account_id
    if login_pin:
        env["LUNARNX_PSN_LOGIN_PIN"] = login_pin
    started_at = time.monotonic()
    process = subprocess.run(
        [str(native_binary), device, str(media_seconds)],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=180,
        check=False,
    )
    secrets = (token, refresh_token, account_id, token_data.get("duid", ""))
    sanitized_lines = [redact_text(line, secrets) for line in process.stdout.splitlines()]
    (output_dir / "01_chiaki_trace.log").write_text(
        "\n".join(sanitized_lines) + "\n")

    phases = []
    for line in sanitized_lines:
        if not line.startswith("TRACE "):
            continue
        try:
            phases.append(json.loads(line[6:]))
        except json.JSONDecodeError:
            phases.append({"phase": "trace_parse", "status": "error"})
    write_json(output_dir / "02_phases.json", phases)

    media_summary = next(
        (phase for phase in reversed(phases)
         if phase.get("phase") == "media_summary"), {})
    summary = {
        "output_dir": str(output_dir),
        "device": device,
        "exit_code": process.returncode,
        "elapsed_ms": round((time.monotonic() - started_at) * 1000, 1),
        "last_phase": phases[-1] if phases else None,
        "media": media_summary,
        "gate": validate_full_flow(
            phases, process.returncode, relay_expected=relay_expected),
    }
    write_json(output_dir / "03_summary.json", summary)
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--token", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, default=Path("/tmp/lunarnx-psn-probes"))
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--listen-seconds", type=float, default=10)
    parser.add_argument("--insecure", action="store_true")
    parser.add_argument("--full", action="store_true",
                        help="Run the native Chiaki PSN-to-media flow")
    parser.add_argument("--native-binary", type=Path,
                        default=Path("/tmp/lunarnx-psn-remote-probe"))
    parser.add_argument("--device", default="PS5-231")
    parser.add_argument("--media-seconds", type=int, default=15)
    parser.add_argument("--login-pin",
                        help="Console login PIN; never written to the trace")
    parser.add_argument("--require-complete", action="store_true",
                        help="Exit nonzero unless the complete media flow passes")
    args = parser.parse_args()

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    output_dir = args.output_root / stamp
    if args.full:
        summary = run_full_probe(
            args.token, output_dir, args.native_binary, args.device,
            args.media_seconds, args.login_pin, args.timeout)
    else:
        summary = asyncio.run(run_probe(
            args.token, output_dir, args.insecure, args.timeout,
            args.listen_seconds))
    print(json.dumps(summary, ensure_ascii=False))
    if args.full and args.require_complete and not summary["gate"]["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
