#!/usr/bin/env python3
"""
Mock Xbox WebRTC Streaming Server

Matches XStreaming's observed Xbox protocol exactly:
  - REST signaling API (session create / SDP / ICE exchange)
  - WebRTC answerer role with a=setup:passive
  - Data channels via DCEP (NOT negotiated) — chat/control/input/message
  - messageV1 handshake with correct CV format
  - authorizationRequest via control channel
  - ServerMetadata via input channel (uint32 width/height)
  - H.264 video + Opus audio streaming from a local file

Usage:
  python3 mock_xbox_server.py --video /tmp/test_stream.mp4
"""

import argparse
from pathlib import Path
import asyncio
import ipaddress
import json
import math
import os
import re
import struct
import sys
import time
import uuid
import logging

from aiohttp import web
from OpenSSL import crypto
from aiortc import (
    RTCPeerConnection,
    RTCSessionDescription,
    MediaStreamTrack,
    RTCConfiguration,
    RTCIceServer,
)
import aiortc.rtcsctptransport as aiortc_sctp
import aiortc.codecs.h264 as aiortc_h264
from fractions import Fraction
from av import VideoFrame
import av

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("mock-xbox")

_sctp_probe_installed = False
_dtls_cert_probe_installed = False
_h264_encoder_patch_installed = False


def install_switch_h264_encoder_patch():
    global _h264_encoder_patch_installed
    if _h264_encoder_patch_installed:
        return
    _h264_encoder_patch_installed = True

    def encode_frame_switch_safe(self, frame, force_keyframe):
        if self.codec and (
            frame.width != self.codec.width
            or frame.height != self.codec.height
            or abs(self.target_bitrate - self.codec.bit_rate) / self.codec.bit_rate > 0.1
        ):
            self.buffer_data = b""
            self.buffer_pts = None
            self.codec = None

        if force_keyframe:
            frame.pict_type = av.video.frame.PictureType.I
        else:
            frame.pict_type = av.video.frame.PictureType.NONE

        if self.codec is None:
            self.codec = av.CodecContext.create("libx264", "w")
            self.codec.width = frame.width
            self.codec.height = frame.height
            self.codec.bit_rate = self.target_bitrate
            self.codec.pix_fmt = "yuv420p"
            self.codec.framerate = Fraction(aiortc_h264.MAX_FRAME_RATE, 1)
            self.codec.time_base = Fraction(1, aiortc_h264.MAX_FRAME_RATE)
            self.codec.options = {
                "level": "31",
                "threads": "1",
                "tune": "zerolatency",
            }
            self.codec.profile = "Baseline"

        data_to_send = b""
        for package in self.codec.encode(frame):
            data_to_send += bytes(package)

        if data_to_send:
            yield from self._split_bitstream(data_to_send)

    aiortc_h264.H264Encoder._encode_frame = encode_frame_switch_safe
    logger.info("[H264] Patched aiortc libx264 encoder: threads=1 for Switch NVDEC")


install_switch_h264_encoder_patch()


def install_dtls_certificate_probe():
    global _dtls_cert_probe_installed
    if _dtls_cert_probe_installed:
        return
    _dtls_cert_probe_installed = True

    from aiortc.rtcdtlstransport import RTCDtlsTransport

    original_validate_peer_identity = RTCDtlsTransport._validate_peer_identity

    def validate_peer_identity_with_dump(self, remoteParameters):
        try:
            return original_validate_peer_identity(self, remoteParameters)
        except Exception:
            try:
                cert = self._ssl.get_peer_certificate(as_cryptography=False)
                der = crypto.dump_certificate(crypto.FILETYPE_ASN1, cert)
                with open("/tmp/lunarnx_peer_cert.der", "wb") as out:
                    out.write(der)
                logger.exception("[DTLS] peer certificate validation failed; dumped %dB to /tmp/lunarnx_peer_cert.der",
                                 len(der))
            except Exception:
                logger.exception("[DTLS] peer certificate validation failed; could not dump certificate")
            raise

    RTCDtlsTransport._validate_peer_identity = validate_peer_identity_with_dump


def install_sctp_probe():
    global _sctp_probe_installed
    if _sctp_probe_installed:
        return
    _sctp_probe_installed = True

    original_parse_packet = aiortc_sctp.parse_packet

    def parse_packet_with_log(data):
        try:
            result = original_parse_packet(data)
        except ValueError as exc:
            logger.warning("[SCTP] parse failed len=%d head=%s error=%s",
                           len(data), data[:16].hex(), exc)
            raise

        source_port, destination_port, verification_tag, chunks = result
        logger.info("[SCTP] recv src=%d dst=%d tag=0x%08x chunks=%s len=%d",
                    source_port,
                    destination_port,
                    verification_tag,
                    ",".join(type(chunk).__name__ for chunk in chunks),
                    len(data))
        return result

    aiortc_sctp.parse_packet = parse_packet_with_log

# ---------------------------------------------------------------------------
# SDP preprocessing — make libpeer's SDP compatible with aiortc
# ---------------------------------------------------------------------------

def preprocess_sdp_offer(sdp: str) -> str:
    """
    Fix libpeer-generated SDP so aiortc can parse it.

    libpeer's H.264 fmtp uses parameter ordering that aiortc rejects:
      libpeer:  profile-level-id=42e01f;level-asymmetry-allowed=1
      aiortc:   level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f

    We rewrite ALL H.264 fmtp lines to match aiortc's expected format.
    Also add packetization-mode=1 and change video direction from
    sendrecv to recvonly (Xbox console only sends video, doesn't receive).
    """
    lines = sdp.split("\r\n")
    h264_payload_types: set[str] = set()
    in_video = False
    for line in lines:
        if line.startswith("m=video"):
            in_video = True
        elif line.startswith("m=") and not line.startswith("m=video"):
            in_video = False
        if in_video and line.startswith("a=rtpmap:") and " H264/" in line.upper():
            h264_payload_types.add(line.split(":", 1)[1].split(" ", 1)[0])

    out = []
    in_video = False

    for line in lines:
        # Track which media section we're in
        if line.startswith("m=video"):
            in_video = True
        elif line.startswith("m=") and not line.startswith("m=video"):
            in_video = False

        if in_video and line == "a=sendrecv":
            out.append("a=recvonly")
            continue

        # Rewrite H.264 fmtp to aiortc's expected parameter order.
        # Do not rewrite RTX fmtp lines such as "apt=97".
        if line.startswith("a=fmtp:") and in_video:
            pt = line.split(":")[1].split(" ")[0]
            if pt not in h264_payload_types and "profile-level-id=" not in line:
                out.append(line)
                continue
            # Parse existing params
            params_str = line[len(f"a=fmtp:{pt} "):]
            params = {}
            for p in params_str.split(";"):
                p = p.strip()
                if "=" in p:
                    k, v = p.split("=", 1)
                    params[k.strip()] = v.strip()

            # Rebuild in aiortc's expected order
            profile = params.get("profile-level-id", "42e01f")
            out.append(
                f"a=fmtp:{pt} "
                f"level-asymmetry-allowed=1;packetization-mode=1;"
                f"profile-level-id={profile}"
            )
            continue

        out.append(line)

    return "\r\n".join(out)


# ---------------------------------------------------------------------------
# Session store
# ---------------------------------------------------------------------------

class Session:
    def __init__(self, session_id: str):
        self.session_id = session_id
        self.state = "Provisioning"
        self.answer_sdp = ""
        self.local_ice: list[dict] = []
        self.pc: RTCPeerConnection | None = None
        self.created_at = time.time()


_sessions: dict[str, Session] = {}
_video_path: str = ""
_public_ip: str = "127.0.0.1"
_ice_shape: str = "local"
_send_test_rumble: bool = False


_BUTTON_PHYSICALITY = (
    (0x0002, 0x00000400),
    (0x0004, 0x00000010),
    (0x0008, 0x00000020),
    (0x0010, 0x00001000),
    (0x0020, 0x00002000),
    (0x0040, 0x00004000),
    (0x0080, 0x00008000),
    (0x0100, 0x00000001),
    (0x0200, 0x00000002),
    (0x0400, 0x00000004),
    (0x0800, 0x00000008),
    (0x1000, 0x00000100),
    (0x2000, 0x00000200),
    (0x4000, 0x00000040),
    (0x8000, 0x00000080),
)


def _expected_gamepad_physicality(button_mask: int,
                                  left_x: int,
                                  left_y: int,
                                  right_x: int,
                                  right_y: int,
                                  left_trigger: int,
                                  right_trigger: int) -> int:
    physicality = 0
    for button, physical_bit in _BUTTON_PHYSICALITY:
        if button_mask & button:
            physicality |= physical_bit
    if left_trigger:
        physicality |= 0x00010000
    if right_trigger:
        physicality |= 0x00020000
    if abs(left_x) > 2000 or abs(left_y) > 2000:
        physicality |= 0x000c0000
    if abs(right_x) > 2000 or abs(right_y) > 2000:
        physicality |= 0x00300000
    return physicality


def parse_input_packet(message: bytes) -> dict:
    if not isinstance(message, bytes) or len(message) < 14:
        raise ValueError("input packet length must be at least 14 bytes")

    report_type, sequence, timestamp = struct.unpack_from("<HId", message, 0)
    if not math.isfinite(timestamp):
        raise ValueError("input packet timestamp must be finite")

    parsed = {
        "report_type": report_type,
        "sequence": sequence,
        "timestamp": timestamp,
    }
    if report_type == 0x08:
        if len(message) != 15:
            raise ValueError("client metadata length must be 15 bytes")
        parsed.update(kind="client_metadata", max_touch_points=message[14])
        return parsed

    if report_type != 0x02:
        raise ValueError(f"unsupported input report type 0x{report_type:04x}")
    if len(message) != 38:
        raise ValueError("gamepad packet length must be 38 bytes")
    if message[14] != 1:
        raise ValueError("gamepad frame count must be one")

    gamepad_index = message[15]
    (button_mask,
     left_x,
     left_y,
     right_x,
     right_y,
     left_trigger,
     right_trigger,
     physicality,
     virtual_physicality) = struct.unpack_from("<HhhhhHHII", message, 16)
    expected_physicality = _expected_gamepad_physicality(
        button_mask,
        left_x,
        left_y,
        right_x,
        right_y,
        left_trigger,
        right_trigger,
    )
    if physicality != expected_physicality:
        raise ValueError(
            f"gamepad physicality 0x{physicality:08x} "
            f"does not match 0x{expected_physicality:08x}"
        )
    if virtual_physicality != 0:
        raise ValueError("gamepad virtual physicality must be zero")

    parsed.update(
        kind="gamepad",
        gamepad_index=gamepad_index,
        button_mask=button_mask,
        left_x=left_x,
        left_y=left_y,
        right_x=right_x,
        right_y=right_y,
        left_trigger=left_trigger,
        right_trigger=right_trigger,
        physicality=physicality,
        virtual_physicality=virtual_physicality,
    )
    return parsed


class InputPacketValidator:
    def __init__(self):
        self._last_sequence: int | None = None

    def validate(self, message: bytes) -> dict:
        parsed = parse_input_packet(message)
        sequence = parsed["sequence"]
        if self._last_sequence is not None:
            expected = (self._last_sequence + 1) & 0xffffffff
            if sequence != expected:
                raise ValueError(
                    f"input sequence {sequence} does not follow {expected}"
                )
        self._last_sequence = sequence
        return parsed


def build_test_rumble_packet() -> bytes:
    metadata = struct.pack("<HII", 0x90, 720, 1280)
    vibration = struct.pack(
        "<BBBBBBHHB",
        0,
        0,
        25,
        50,
        75,
        100,
        250,
        0,
        0,
    )
    return metadata + vibration


# ---------------------------------------------------------------------------
# XStreaming response shaping
# ---------------------------------------------------------------------------

_CANDIDATE_RE = re.compile(
    r"^(?:a=)?candidate:(?P<foundation>\S+)\s+"
    r"(?P<component>\d+)\s+"
    r"(?P<protocol>\S+)\s+"
    r"(?P<priority>\d+)\s+"
    r"(?P<ip>\S+)\s+"
    r"(?P<port>\d+)\s+"
    r"(?P<rest>.*)$",
    re.IGNORECASE,
)


def shape_sdp_answer_for_xbox(sdp: str) -> str:
    """Match Xbox /sdp answer shape and keep only libpeer-compatible fields."""
    out: list[str] = []
    for line in sdp.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith("a=candidate:") or stripped == "a=end-of-candidates":
            continue
        if stripped.startswith("a=fingerprint:") and not stripped.startswith("a=fingerprint:sha-256 "):
            continue
        if stripped.startswith("a=setup:"):
            ending = "\r\n" if line.endswith("\r\n") else "\n" if line.endswith("\n") else ""
            out.append(f"a=setup:passive{ending}")
            continue
        out.append(line)
    return "".join(out)


def strip_inline_ice_candidates(sdp: str) -> str:
    """Xbox returns candidates from /ice, not inline in the /sdp answer."""
    return shape_sdp_answer_for_xbox(sdp)


def _normalize_ip(value: str) -> str:
    value = (value or "").strip()
    if not value:
        return ""
    if value.startswith("[") and value.endswith("]"):
        value = value[1:-1]
    return value.split("%", 1)[0]


def _host_header_ip(request: web.Request) -> str:
    host = request.host.split(":", 1)[0]
    return _normalize_ip(host)


def _candidate_dict(candidate: str, sdp_mid: str = "0", sdp_mline_index: int = 0) -> dict | None:
    match = _CANDIDATE_RE.match(candidate.strip())
    if not match:
        return None

    ip = _normalize_ip(match.group("ip"))
    try:
        ip_obj = ipaddress.ip_address(ip)
    except ValueError:
        return None

    if ip_obj.version != 4:
        return None
    if ip_obj.is_loopback or ip_obj.is_unspecified or ip_obj.is_multicast:
        return None

    return {
        "candidate": candidate.strip(),
        "sdpMid": str(sdp_mid or "0"),
        "sdpMLineIndex": int(sdp_mline_index or 0),
        "ip": ip,
        "port": int(match.group("port")),
        "protocol": match.group("protocol").upper(),
        "priority": int(match.group("priority")),
        "rest": match.group("rest").strip(),
    }


def parse_remote_ice_candidate(item: dict):
    if not isinstance(item, dict):
        return None
    cand_str = (item.get("candidate") or "").strip()
    if not cand_str or cand_str == "a=end-of-candidates":
        return None

    lowered = cand_str.lower()
    if " udp " in lowered and "tcptype" in lowered:
        return None

    if cand_str.startswith("a=candidate:"):
        cand_str = cand_str[len("a=candidate:"):]
    elif cand_str.startswith("candidate:"):
        cand_str = cand_str[len("candidate:"):]

    from aiortc.sdp import candidate_from_sdp

    candidate = candidate_from_sdp(cand_str)
    candidate.sdpMid = str(item.get("sdpMid", "0") or "0")
    candidate.sdpMLineIndex = int(item.get("sdpMLineIndex", 0) or 0)
    return candidate


def _extract_inline_candidates(sdp: str) -> list[dict]:
    candidates: list[dict] = []
    current_mid = "0"
    current_mline = -1

    for raw in sdp.splitlines():
        line = raw.strip()
        if line.startswith("m="):
            current_mline += 1
            continue
        if line.startswith("a=mid:"):
            current_mid = line.split(":", 1)[1] or "0"
            continue
        if not line.startswith("a=candidate:"):
            continue
        parsed = _candidate_dict(line, "0", 0)
        if parsed:
            parsed["sourceMid"] = current_mid
            parsed["sourceMLineIndex"] = max(current_mline, 0)
            candidates.append(parsed)

    return candidates


def _score_candidate(candidate: dict, preferred_ip: str) -> tuple[int, int, int]:
    ip = candidate["ip"]
    ip_obj = ipaddress.ip_address(ip)
    if preferred_ip and ip == preferred_ip:
        return (0, -candidate["priority"], candidate["port"])
    if ip_obj.is_private:
        penalty = 20 if ip.startswith("172.") else 10
        if ip.endswith(".0"):
            penalty += 50
        return (penalty, -candidate["priority"], candidate["port"])
    return (40, -candidate["priority"], candidate["port"])


def _shape_ice_candidates(candidates: list[dict], preferred_ip: str = "") -> list[dict]:
    preferred_ip = _normalize_ip(preferred_ip)
    parsed: list[dict] = []
    for item in candidates:
        if not isinstance(item, dict):
            continue
        parsed_item = _candidate_dict(
            item.get("candidate", ""),
            item.get("sdpMid", "0"),
            item.get("sdpMLineIndex", 0),
        )
        if parsed_item:
            parsed.append(parsed_item)

    if preferred_ip and any(item["ip"] == preferred_ip for item in parsed):
        parsed = [item for item in parsed if item["ip"] == preferred_ip]

    parsed.sort(key=lambda item: _score_candidate(item, preferred_ip))

    shaped: list[dict] = []
    seen: set[tuple[str, int, str]] = set()
    foundation = 1
    for item in parsed:
        key = (item["ip"], item["port"], item["rest"])
        if key in seen:
            continue
        seen.add(key)
        priority = 2130706431 if foundation == 1 else 1
        shaped.append({
            "candidate": (
                f"a=candidate:{foundation} 1 UDP {priority} "
                f"{item['ip']} {item['port']} {item['rest']}"
            ),
            "messageType": "iceCandidate",
            "sdpMLineIndex": 0,
            "sdpMid": "0",
        })
        foundation += 1

    shaped.append({
        "candidate": "a=end-of-candidates",
        "messageType": "iceCandidate",
        "sdpMLineIndex": 0,
        "sdpMid": "0",
    })
    return shaped


def _teredo_ipv6_for_client(client_ip: str, mapped_port: int) -> str:
    server = int(ipaddress.IPv4Address("23.103.64.2"))
    client = int(ipaddress.IPv4Address(client_ip))
    obfuscated_port = (~int(mapped_port)) & 0xffff
    obfuscated_client = (~client) & 0xffffffff

    return (
        "2001:0000:"
        f"{(server >> 16) & 0xffff:04x}:"
        f"{server & 0xffff:04x}:"
        "2824:"
        f"{obfuscated_port:04x}:"
        f"{(obfuscated_client >> 16) & 0xffff:04x}:"
        f"{obfuscated_client & 0xffff:04x}"
    )


def _select_reachable_candidate(candidates: list[dict], preferred_ip: str = "") -> dict | None:
    parsed: list[dict] = []
    for item in candidates:
        if not isinstance(item, dict):
            continue
        parsed_item = _candidate_dict(
            item.get("candidate", ""),
            item.get("sdpMid", "0"),
            item.get("sdpMLineIndex", 0),
        )
        if parsed_item:
            parsed.append(parsed_item)

    preferred_ip = _normalize_ip(preferred_ip)
    if preferred_ip:
        preferred = [item for item in parsed if item["ip"] == preferred_ip]
        if preferred:
            parsed = preferred

    if not parsed:
        return None

    parsed.sort(key=lambda item: _score_candidate(item, preferred_ip))
    return parsed[0]


def _shape_xbox_teredo_ice_candidates(candidates: list[dict],
                                      preferred_ip: str = "") -> list[dict]:
    selected = _select_reachable_candidate(candidates, preferred_ip)
    if not selected:
        return _shape_ice_candidates(candidates, preferred_ip)

    teredo_ip = _teredo_ipv6_for_client(selected["ip"], selected["port"])
    return [
        {
            "candidate": "a=candidate:1 1 UDP 100 192.168.1.10 9002 typ host ",
            "messageType": "iceCandidate",
            "sdpMLineIndex": 0,
            "sdpMid": "0",
        },
        {
            "candidate": (
                "a=candidate:2 1 UDP 1 "
                "2001:db8:1::11 9002 typ host "
            ),
            "messageType": "iceCandidate",
            "sdpMLineIndex": 0,
            "sdpMid": "0",
        },
        {
            "candidate": f"a=candidate:3 1 UDP 1 {teredo_ip} 9002 typ host ",
            "messageType": "iceCandidate",
            "sdpMLineIndex": 0,
            "sdpMid": "0",
        },
        {
            "candidate": "a=end-of-candidates",
            "messageType": "iceCandidate",
            "sdpMLineIndex": 0,
            "sdpMid": "0",
        },
    ]


def build_sdp_exchange_response(sdp: str) -> dict:
    inner = {
        "chat": 1,
        "chatConfiguration": {
            "format": {
                "codec": "opus",
                "container": "webm",
            },
        },
        "control": 3,
        "input": 8,
        "message": 1,
        "messageType": "answer",
        "sdp": shape_sdp_answer_for_xbox(sdp),
        "sdpType": "answer",
        "status": "success",
    }
    return {
        "exchangeResponse": json.dumps(inner),
        "errorDetails": {
            "code": None,
            "message": None,
        },
    }


def build_ice_exchange_response(candidates: list[dict],
                                preferred_ip: str = "",
                                ice_shape: str = "local") -> dict:
    shaped = (
        _shape_xbox_teredo_ice_candidates(candidates, preferred_ip)
        if ice_shape == "xbox-teredo"
        else _shape_ice_candidates(candidates, preferred_ip)
    )
    return {
        "exchangeResponse": json.dumps(shaped),
        "errorDetails": {
            "code": None,
            "message": None,
        },
    }


# ---------------------------------------------------------------------------
# Media tracks
# ---------------------------------------------------------------------------

async def _pace_media(start_time_attr, media_time: float) -> None:
    if start_time_attr[0] is None:
        start_time_attr[0] = time.time() - media_time
        return
    wait = start_time_attr[0] + media_time - time.time()
    if wait > 0:
        await asyncio.sleep(wait)


class VideoFileTrack(MediaStreamTrack):
    kind = "video"

    def __init__(self, path: str):
        super().__init__()
        self._container = av.open(path, "r")
        self._stream = self._container.streams.video[0]
        self._stream.thread_type = "AUTO"
        self._frame_count = 0
        fps = float(self._stream.average_rate) if self._stream.average_rate else 30.0
        self._pts_step = int(90000 / fps)
        self._pace_start = [None]
        logger.info(f"Video: {self._stream.width}x{self._stream.height} "
                    f"codec={self._stream.codec_context.name} fps={fps}")

    def _wrap_video_frame(self, frame) -> VideoFrame:
        img = frame.to_ndarray(format="yuv420p")
        vf = VideoFrame.from_ndarray(img, format="yuv420p")
        vf.pts = self._frame_count * self._pts_step
        vf.time_base = Fraction(1, 90000)
        self._frame_count += 1
        return vf

    def _next_decoded_video_frame(self) -> VideoFrame | None:
        for packet in self._container.demux(self._stream):
            try:
                frames = packet.decode()
            except av.error.EOFError:
                return None
            except av.error.FFmpegError:
                continue

            for frame in frames:
                return self._wrap_video_frame(frame)
        return None

    def _restart_video(self) -> None:
        self._container.seek(0)

    async def recv(self) -> VideoFrame:
        frame = self._next_decoded_video_frame()
        if frame:
            await _pace_media(self._pace_start, float(frame.pts * frame.time_base))
            return frame

        self._restart_video()
        self._pace_start[0] = None
        frame = self._next_decoded_video_frame()
        if frame:
            await _pace_media(self._pace_start, float(frame.pts * frame.time_base))
            return frame

        # Fallback black frame
        vf = VideoFrame(width=1280, height=720, format="yuv420p")
        vf.pts = 0
        vf.time_base = Fraction(1, 90000)
        await _pace_media(self._pace_start, 0.0)
        return vf


class AudioFileTrack(MediaStreamTrack):
    kind = "audio"

    def __init__(self, path: str):
        super().__init__()
        self._container = av.open(path, "r")
        self._stream = self._container.streams.audio[0]
        self._resampler = av.AudioResampler(format="s16", layout="stereo", rate=48000)
        self._sample_rate = 48000
        self._samples = 0
        self._pace_start = [None]
        logger.info(f"Audio: codec={self._stream.codec_context.name} "
                    f"rate={self._stream.rate}")

    async def recv(self) -> av.AudioFrame:
        for packet in self._container.demux(self._stream):
            try:
                for frame in packet.decode():
                    resampled = self._resampler.resample(frame)
                    if resampled:
                        rf = resampled[0]
                        rf.pts = self._samples
                        rf.sample_rate = self._sample_rate
                        rf.time_base = Fraction(1, 48000)
                        self._samples += rf.samples
                        await _pace_media(self._pace_start, float(rf.pts * rf.time_base))
                        return rf
            except Exception:
                continue
        # Loop
        self._container.seek(0)
        self._pace_start[0] = None
        for packet in self._container.demux(self._stream):
            try:
                for frame in packet.decode():
                    resampled = self._resampler.resample(frame)
                    if resampled:
                        rf = resampled[0]
                        rf.pts = self._samples
                        rf.sample_rate = self._sample_rate
                        rf.time_base = Fraction(1, 48000)
                        self._samples += rf.samples
                        await _pace_media(self._pace_start, float(rf.pts * rf.time_base))
                        return rf
            except Exception:
                continue
        # Silence
        rf = av.AudioFrame(format="s16", layout="stereo", samples=960)
        rf.pts = self._samples
        rf.sample_rate = self._sample_rate
        rf.time_base = Fraction(1, 48000)
        self._samples += 960
        await _pace_media(self._pace_start, float(rf.pts * rf.time_base))
        return rf


# ---------------------------------------------------------------------------
# WebRTC session (server = answerer = Xbox role)
# ---------------------------------------------------------------------------

async def create_webrtc_answer(session: Session) -> str:
    config = RTCConfiguration(
        iceServers=[RTCIceServer(urls="stun:stun.l.google.com:19302")]
    )
    pc = RTCPeerConnection(configuration=config)
    session.pc = pc
    video_sender = None

    # --- Setup ICE / connection state logging ---
    @pc.on("icecandidate")
    def on_icecandidate(candidate):
        if candidate:
            cand_raw = candidate.candidate
            if cand_raw and not cand_raw.startswith("candidate:"):
                cand_raw = f"candidate:{cand_raw}"
            logger.info(f"[ICE] Local: {cand_raw[:80]}...")
            session.local_ice.append({
                "candidate": cand_raw,
                "sdpMid": candidate.sdpMid or "0",
                "sdpMLineIndex": candidate.sdpMLineIndex if candidate.sdpMLineIndex is not None else 0,
            })

    @pc.on("iceconnectionstatechange")
    async def on_ice():
        logger.info(f"[ICE] State: {pc.iceConnectionState}")

    @pc.on("connectionstatechange")
    async def on_conn():
        logger.info(f"[WebRTC] State: {pc.connectionState}")

    # --- Handle incoming data channels (DCEP, NOT negotiated) ---
    @pc.on("datachannel")
    def on_datachannel(channel):
        label = channel.label
        logger.info(f"[DC] Incoming channel: {label} (id={channel.id})")

        if label == "message":
            _setup_message_channel(channel)
        elif label == "control":
            _setup_control_channel(channel, video_sender)
        elif label == "input":
            _setup_input_channel(channel)
        elif label == "chat":
            _setup_chat_channel(channel)

    # --- Set remote offer (with preprocessing for libpeer compat) ---
    sdp = preprocess_sdp_offer(session.offer_sdp)
    logger.info(f"[SDP] Offer ({len(sdp)} bytes, preprocessed)")
    offer = RTCSessionDescription(sdp=sdp, type="offer")
    await pc.setRemoteDescription(offer)

    # --- Add media tracks ---
    video_sender = pc.addTrack(VideoFileTrack(_video_path))
    pc.addTrack(AudioFileTrack(_video_path))

    # --- Create answer ---
    answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)

    sdp = pc.localDescription.sdp
    if "a=setup:passive" not in sdp:
        sdp = re.sub(r"a=setup:(?:active|actpass)", "a=setup:passive", sdp)
    session.answer_sdp = sdp
    inline_candidates = _extract_inline_candidates(sdp)
    if inline_candidates:
        session.local_ice = inline_candidates

    logger.info(f"[SDP] Answer ready ({len(sdp)} bytes, candidates={len(session.local_ice)})")
    return sdp


# --- Data channel protocol handlers (matching XStreaming exactly) ---

def _setup_message_channel(channel):
    """messageV1 protocol — XStreaming src/webrtc/Channel/Message.ts"""

    @channel.on("message")
    def on_msg(message):
        text = message if isinstance(message, str) else message.decode()
        logger.info(f"[MSG] Recv: {text[:200]}")
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return

        if data.get("type") == "Handshake":
            # Build HandshakeAck with proper CV format
            cv = data.get("cv", "0.1")
            ack = json.dumps({
                "type": "HandshakeAck",
                "version": "messageV1",
                "id": str(uuid.uuid4()),
                "cv": cv,
            })
            channel.send(ack)
            logger.info("[MSG] Sent HandshakeAck")

            # After handshake, the real Xbox sends nothing further here.
            # System UI config etc. are client->server in XStreaming.
        else:
            # Forward unknown message types (Message, TransactionStart, etc.)
            logger.info(f"[MSG] Unhandled type: {data.get('type')}")

    @channel.on("open")
    def _():
        logger.info("[MSG] Opened")


def _setup_control_channel(channel, video_sender=None):
    """controlV1 protocol — XStreaming src/webrtc/Channel/Control.ts"""

    @channel.on("message")
    def on_ctrl(message):
        text = message if isinstance(message, str) else message.decode()
        logger.info(f"[CTRL] Recv: {text}")
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return

        msg_type = data.get("message", "")
        if msg_type == "authorizationRequest":
            key = data.get("accessKey", "")
            logger.info(f"[CTRL] Auth request (key={key[:20]}...) — accepted")
        elif msg_type == "gamepadChanged":
            logger.info(f"[CTRL] Gamepad {data.get('gamepadIndex')} "
                        f"added={data.get('wasAdded')}")
        elif msg_type == "videoKeyframeRequested":
            logger.info(f"[CTRL] Keyframe requested: {data.get('ifrRequested')}")
            if video_sender and hasattr(video_sender, "_send_keyframe"):
                video_sender._send_keyframe()
                logger.info("[CTRL] Forced aiortc video keyframe")
        else:
            logger.info(f"[CTRL] Unknown message: {msg_type}")

    @channel.on("open")
    def _():
        logger.info("[CTRL] Opened")


def _setup_input_channel(channel):
    """input channel (protocol 1.0) — XStreaming src/webrtc/Channel/Input.ts"""
    validator = InputPacketValidator()
    last_gamepad_state = None
    rumble_sent = False

    @channel.on("message")
    def on_input(message):
        nonlocal last_gamepad_state, rumble_sent
        if not isinstance(message, bytes):
            logger.error("[INPUT] Expected binary packet")
            return
        try:
            parsed = validator.validate(message)
        except ValueError as exc:
            logger.error("[INPUT] Invalid packet: %s", exc)
            return

        if parsed["kind"] == "client_metadata":
            logger.info("[INPUT] ClientMetadata sequence=%d", parsed["sequence"])
            channel.send(struct.pack("<HII", 0x10, 720, 1280))
            return

        gamepad_state = (
            parsed["button_mask"],
            parsed["left_x"],
            parsed["left_y"],
            parsed["right_x"],
            parsed["right_y"],
            parsed["left_trigger"],
            parsed["right_trigger"],
            parsed["physicality"],
        )
        if gamepad_state != last_gamepad_state:
            logger.info(
                "[INPUT] Gamepad sequence=%d buttons=0x%04x "
                "left=(%d,%d) right=(%d,%d) triggers=(%d,%d) physicality=0x%08x",
                parsed["sequence"],
                parsed["button_mask"],
                parsed["left_x"],
                parsed["left_y"],
                parsed["right_x"],
                parsed["right_y"],
                parsed["left_trigger"],
                parsed["right_trigger"],
                parsed["physicality"],
            )
            last_gamepad_state = gamepad_state
        if _send_test_rumble and not rumble_sent:
            channel.send(build_test_rumble_packet())
            rumble_sent = True
            logger.info("[INPUT] Sent deterministic test rumble")

    @channel.on("open")
    def _():
        logger.info("[INPUT] Opened")


def _setup_chat_channel(channel):
    """chatV1 — XStreaming src/webrtc/Channel/Chat.ts"""

    @channel.on("message")
    def on_chat(message):
        logger.debug(f"[CHAT] {len(message) if isinstance(message, bytes) else len(str(message))}B")

    @channel.on("open")
    def _():
        logger.info("[CHAT] Opened")


# ---------------------------------------------------------------------------
# HTTP REST API — matches XStreaming's observed Xbox GSSV endpoints
# ---------------------------------------------------------------------------

async def handle_consoles(request: web.Request) -> web.Response:
    """GET /v6/servers/home"""
    return web.json_response({
        "results": [{
            "serverId": "MOCKXBOX001",
            "deviceName": "Mock-Xbox-Series-X",
            "consoleType": "XboxSeriesX",
            "powerState": "On",
            "locale": "en-US",
        }],
    })


async def handle_create_session(request: web.Request) -> web.Response:
    """POST /v5/sessions/home/play"""
    body = await request.json()
    server_id = body.get("serverId", "unknown")
    logger.info(f"[API] Create session for {server_id}")

    session_id = str(uuid.uuid4())
    session = Session(session_id)
    _sessions[session_id] = session

    # Real Xbox: Provisioning -> (1-3s) -> Provisioned
    loop = asyncio.get_running_loop()
    loop.call_later(1.0, lambda: setattr(session, "state", "Provisioned"))

    return web.json_response({
        "sessionId": session_id,
        "sessionPath": f"v5/sessions/home/{session_id}",
        "state": "Provisioning",
    }, status=201)


async def handle_session_state(request: web.Request) -> web.Response:
    """GET /v5/sessions/home/{sessionId}/state"""
    sid = request.match_info["sessionId"]
    s = _sessions.get(sid)
    if not s:
        return web.json_response({"error": "Session not found"}, status=404)
    return web.json_response({"state": s.state})


async def handle_session_config(request: web.Request) -> web.Response:
    """GET /v5/sessions/home/{sessionId}/configuration"""
    sid = request.match_info["sessionId"]
    s = _sessions.get(sid)
    if not s:
        return web.json_response({"error": "Session not found"}, status=404)

    reported_ip = _public_ip
    request_ip = _host_header_ip(request)
    if reported_ip in ("", "127.0.0.1", "localhost") and request_ip not in ("", "127.0.0.1"):
        reported_ip = request_ip

    # Matches XStreaming docs/3.Stream.md exact format
    return web.json_response({
        "keepAlivePulseInSeconds": 300,
        "serverDetails": {
            "ipAddress": reported_ip,
            "port": 9002,
            "ipV4Address": reported_ip,
            "ipV4Port": 9002,
            "ipV6Address": None,
            "ipV6Port": 0,
            "iceExchangePath": f"v5/sessions/home/{sid}/ice",
            "stunServerAddress": None,
            "srtp": {
                "key": "KqZGbZ0cXCvtxzD9z/63GAl2pfu+nB2maykK7Vbq",
            },
        },
    })


async def handle_sdp_post(request: web.Request) -> web.Response:
    """POST /v5/sessions/home/{sessionId}/sdp"""
    sid = request.match_info["sessionId"]
    s = _sessions.get(sid)
    if not s:
        return web.json_response({"error": "Session not found"}, status=404)

    body = await request.json()
    sdp_offer = body.get("sdp", "")
    logger.info(f"[API] SDP offer ({len(sdp_offer)} bytes)")
    logger.info(f"[SDP] Raw offer:\n{sdp_offer}")

    s.offer_sdp = sdp_offer

    try:
        await create_webrtc_answer(s)
        logger.info("[WebRTC] Answer ready")
    except Exception as e:
        logger.error(f"[WebRTC] Answer failed: {e}", exc_info=True)
        return web.json_response({"error": str(e)}, status=500)

    return web.json_response({}, status=200)


async def handle_sdp_get(request: web.Request) -> web.Response:
    """GET /v5/sessions/home/{sessionId}/sdp"""
    sid = request.match_info["sessionId"]
    s = _sessions.get(sid)
    if not s:
        return web.json_response({"error": "Session not found"}, status=404)

    if not s.answer_sdp:
        return web.json_response({"exchangeResponse": ""})

    return web.json_response(build_sdp_exchange_response(s.answer_sdp))


async def handle_ice_post(request: web.Request) -> web.Response:
    """POST /v5/sessions/home/{sessionId}/ice"""
    sid = request.match_info["sessionId"]
    s = _sessions.get(sid)
    if not s:
        return web.json_response({"error": "Session not found"}, status=404)

    body = await request.json()

    # XStreaming sends: { "messageType": "iceCandidate", "candidate": [...] }
    # LunarNX sends: { "iceCandidates": [...] }
    candidates = body.get("candidate", body.get("iceCandidates", []))
    if isinstance(candidates, dict):
        candidates = [candidates]

    logger.info(f"[API] ICE post: {len(candidates)} candidates")

    for item in candidates:
        if not isinstance(item, dict):
            continue
        cand_str = item.get("candidate", "")
        if not cand_str or cand_str == "a=end-of-candidates":
            continue

        try:
            candidate = parse_remote_ice_candidate(item)
            if candidate:
                await s.pc.addIceCandidate(candidate)
        except Exception as e:
            logger.warning(f"[ICE] Failed to add remote: {e}")

    return web.json_response({}, status=200)


async def handle_ice_get(request: web.Request) -> web.Response:
    """GET /v5/sessions/home/{sessionId}/ice"""
    sid = request.match_info["sessionId"]
    s = _sessions.get(sid)
    if not s:
        return web.json_response({"error": "Session not found"}, status=404)

    if not s.local_ice:
        return web.json_response({"exchangeResponse": ""})

    candidates = []
    for c in s.local_ice:
        candidates.append({
            "candidate": c["candidate"],
            "sdpMid": c["sdpMid"],
            "sdpMLineIndex": c["sdpMLineIndex"],
            "messageType": "iceCandidate",
        })
    s.local_ice = []

    preferred_ip = _host_header_ip(request)
    response = build_ice_exchange_response(candidates,
                                           preferred_ip=preferred_ip,
                                           ice_shape=_ice_shape)
    shaped = json.loads(response["exchangeResponse"])
    logger.info(f"[API] ICE get: raw={len(candidates)} shaped={len(shaped)} preferred={preferred_ip} shape={_ice_shape}")
    return web.json_response(response)


async def handle_delete_session(request: web.Request) -> web.Response:
    """DELETE /v5/sessions/home/{sessionId}"""
    sid = request.match_info["sessionId"]
    s = _sessions.pop(sid, None)
    if s and s.pc:
        await s.pc.close()
    logger.info(f"[API] Session {sid} deleted")
    return web.json_response({}, status=200)



# ---------------------------------------------------------------------------
# xCloud catalog + cloud session mock (for UI / library testing)
# ---------------------------------------------------------------------------

_OFFICIAL_PRODUCT_IDS: list[str] = []
_CLOUD_LIBRARY: list[dict] = []
_CLOUD_BY_PRODUCT: dict[str, dict] = {}
_CLOUD_RECENT_IDS: list[str] = []
_CLOUD_NEW_IDS: list[str] = []


def _load_official_product_ids() -> list[str]:
    candidates = [
        Path(__file__).resolve().parents[2] / "res" / "xcloud_titles.json",
        Path(__file__).resolve().parents[2] / "github_repos" / "XStreaming" / "titles.json",
        Path(__file__).resolve().parents[2] / "romfs" / "xcloud_titles.json",
    ]
    for path in candidates:
        try:
            if not path.exists():
                continue
            data = json.loads(path.read_text())
            products = data.get("Products") or []
            ids = [str(x).upper() for x in products if x]
            if ids:
                logger.info("[xCloud] loaded %d official product ids from %s", len(ids), path)
                return ids
        except Exception as exc:
            logger.warning("[xCloud] failed loading %s: %s", path, exc)
    # Fallback tiny set so mock still works without titles.json.
    return [
        "9NP1P1WFS0LB",  # Halo Infinite-ish sample
        "9NKX70BBCDRN",  # Forza sample
        "9P97ZTRTH25W",
        "9PCM4CMTPPGC",
        "9P4KMR76PLLQ",
    ]



# 1x1 JPEG (valid, tiny) used as local poster payload for emulator stability.
_TINY_JPEG = bytes([
    0xFF,0xD8,0xFF,0xE0,0x00,0x10,0x4A,0x46,0x49,0x46,0x00,0x01,0x01,0x00,0x00,0x01,
    0x00,0x01,0x00,0x00,0xFF,0xDB,0x00,0x43,0x00,0x08,0x06,0x06,0x07,0x06,0x05,0x08,
    0x07,0x07,0x07,0x09,0x09,0x08,0x0A,0x0C,0x14,0x0D,0x0C,0x0B,0x0B,0x0C,0x19,0x12,
    0x13,0x0F,0x14,0x1D,0x1A,0x1F,0x1E,0x1D,0x1A,0x1C,0x1C,0x20,0x24,0x2E,0x27,0x20,
    0x22,0x2C,0x23,0x1C,0x1C,0x28,0x37,0x29,0x2C,0x30,0x31,0x34,0x34,0x34,0x1F,0x27,
    0x39,0x3D,0x38,0x32,0x3C,0x2E,0x33,0x34,0x32,0xFF,0xC0,0x00,0x0B,0x08,0x00,0x01,
    0x00,0x01,0x01,0x01,0x11,0x00,0xFF,0xC4,0x00,0x14,0x00,0x01,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0xFF,0xC4,0x00,0x14,
    0x10,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0xFF,0xDA,0x00,0x08,0x01,0x01,0x00,0x00,0x3F,0x00,0x7F,0xFF,0xD9
])

async def handle_mock_poster(request: web.Request) -> web.Response:
    """Local colored poster endpoint for emulator visibility."""
    name = request.match_info.get("name", "poster")
    colors = ["red", "green", "blue", "orange", "purple", "teal"]
    # stable color from name
    idx = sum(ord(c) for c in name) % len(colors)
    color = colors[idx]
    poster_path = Path(__file__).resolve().parent / "posters" / f"{color}.png"
    if poster_path.exists():
        data = poster_path.read_bytes()
        return web.Response(body=data, content_type="image/png",
                            headers={"Cache-Control": "public, max-age=3600"})
    # fallback 1x1 png
    return web.Response(body=bytes.fromhex(
        '89504e470d0a1a0a0000000d4948445200000001000000010802000000907753de0000000c4944415408d763f8ffff3f0005fe02fe a75b1e0000000049454e44ae426082'.replace(' ','')
    ), content_type="image/png")


def _make_title(product_id: str, idx: int) -> dict:
    # Deterministic fake title metadata for UI rendering.
    product_id = product_id.upper()
    title_id = f"MOCKTITLE{idx:04d}"
    name = f"Mock Cloud Game {idx:03d}"
    # Public placeholder posters (HTTPS). UI will lazy-load them.
    # Using picsum with stable seed for repeatable images.
    poster = f"/mock/poster/{product_id}.jpg"
    return {
        "titleId": title_id,
        "XCloudTitleId": title_id,
        "productId": product_id,
        "ProductTitle": name,
        "PublisherName": "LunarNX Mock Studios",
        "Image_Poster": {"URL": poster},
        "Image_Tile": {"URL": poster},
        "details": {
            "productId": product_id,
            "titleId": title_id,
            "hasEntitlement": True,
            "supportedInputTypes": ["Controller"],
            "programs": ["GPULTIMATE"],
        },
    }


def init_cloud_catalog(max_titles: int = 80) -> None:
    global _OFFICIAL_PRODUCT_IDS, _CLOUD_LIBRARY, _CLOUD_BY_PRODUCT
    global _CLOUD_RECENT_IDS, _CLOUD_NEW_IDS

    ids = _load_official_product_ids()
    if max_titles > 0:
        ids = ids[:max_titles]
    _OFFICIAL_PRODUCT_IDS = ids

    library = []
    by_product = {}
    for i, pid in enumerate(ids, start=1):
        title = _make_title(pid, i)
        library.append(title)
        by_product[pid] = title

    _CLOUD_LIBRARY = library
    _CLOUD_BY_PRODUCT = by_product
    # Recent = first 8, New = next 8 (or wrap).
    _CLOUD_RECENT_IDS = ids[:8]
    _CLOUD_NEW_IDS = ids[8:16] if len(ids) > 8 else ids[: min(8, len(ids))]
    logger.info("[xCloud] catalog ready titles=%d recent=%d new=%d",
                len(_CLOUD_LIBRARY), len(_CLOUD_RECENT_IDS), len(_CLOUD_NEW_IDS))


async def handle_cloud_titles(request: web.Request) -> web.Response:
    """GET /v2/titles  (cloud library raw)"""
    results = []
    for t in _CLOUD_LIBRARY:
        results.append({
            "titleId": t["titleId"],
            "details": t["details"],
        })
    return web.json_response({
        "eTag": "MOCK-CLOUD",
        "totalItems": len(results),
        "results": results,
    })


async def handle_cloud_titles_mru(request: web.Request) -> web.Response:
    """GET /v2/titles/mru?mr=25"""
    mr = 25
    try:
        mr = int(request.rel_url.query.get("mr", "25"))
    except Exception:
        pass
    results = []
    for pid in _CLOUD_RECENT_IDS[:mr]:
        t = _CLOUD_BY_PRODUCT.get(pid)
        if not t:
            continue
        results.append({
            "titleId": t["titleId"],
            "details": t["details"],
            "titleHistory": {"lastTimePlayed": "2026-07-01T00:00:00Z"},
        })
    return web.json_response({"results": results})


async def handle_catalog_products(request: web.Request) -> web.Response:
    """POST catalog.gamepass.com/v3/products (path-agnostic mock)"""
    try:
        body = await request.json()
    except Exception:
        body = {}
    products = body.get("Products") or body.get("products") or []
    out = {}
    for raw in products:
        pid = str(raw).upper()
        t = _CLOUD_BY_PRODUCT.get(pid)
        if not t:
            # synthesize on demand so official-id hydration still works
            t = _make_title(pid, abs(hash(pid)) % 10000)
            _CLOUD_BY_PRODUCT[pid] = t
        poster = t["Image_Poster"]["URL"]
        if poster.startswith("/"):
            host = request.headers.get("Host") or f"{request.url.host}:{request.url.port}"
            scheme = request.scheme or "http"
            poster = f"{scheme}://{host}{poster}"
        out[pid] = {
            "ProductTitle": t["ProductTitle"],
            "PublisherName": t["PublisherName"],
            "titleId": t["titleId"],
            "XCloudTitleId": t["XCloudTitleId"],
            "Image_Poster": {"URL": poster},
            "Image_Tile": {"URL": poster},
        }
    return web.json_response({"Products": out})


async def handle_new_titles_sigls(request: web.Request) -> web.Response:
    """GET catalog.gamepass.com/sigls/v2?id=..."""
    payload = [{
        "siglId": "f13cf6b4-57e6-4459-89df-6aec18cf0538",
        "title": "Recently added",
        "description": "Mock newly added cloud games",
    }]
    for pid in _CLOUD_NEW_IDS:
        payload.append({"id": pid})
    return web.json_response(payload)


async def handle_official_titles_json(request: web.Request) -> web.Response:
    """GET mock of XStreaming titles.json"""
    return web.json_response({"Products": _OFFICIAL_PRODUCT_IDS})


async def handle_cloud_create_session(request: web.Request) -> web.Response:
    """POST /v5/sessions/cloud/play"""
    body = await request.json()
    title_id = body.get("titleId", "")
    logger.info("[API] Create CLOUD session titleId=%s", title_id)

    session_id = str(uuid.uuid4())
    session = Session(session_id)
    # Cloud often goes ReadyToConnect then Provisioned.
    session.state = "ReadyToConnect"
    _sessions[session_id] = session

    loop = asyncio.get_running_loop()

    def to_provisioned():
        # If client never hits /connect, still advance after a bit.
        if session.state in ("ReadyToConnect", "Provisioning"):
            session.state = "Provisioned"

    loop.call_later(8.0, to_provisioned)

    return web.json_response({
        "sessionId": session_id,
        "sessionPath": f"v5/sessions/cloud/{session_id}",
        "state": session.state,
    }, status=201)


async def handle_cloud_connect(request: web.Request) -> web.Response:
    """POST /v5/sessions/cloud/{sessionId}/connect  (ReadyToConnect)"""
    sid = request.match_info["sessionId"]
    s = _sessions.get(sid)
    if not s:
        return web.json_response({"error": "Session not found"}, status=404)
    try:
        body = await request.json()
    except Exception:
        body = {}
    logger.info("[API] Cloud connect session=%s token_len=%s",
                sid, len(str(body.get("userToken", ""))))
    s.state = "Provisioning"

    loop = asyncio.get_running_loop()
    loop.call_later(1.0, lambda: setattr(s, "state", "Provisioned"))
    return web.json_response({}, status=200)


async def handle_keepalive(request: web.Request) -> web.Response:
    sid = request.match_info["sessionId"]
    if sid not in _sessions:
        return web.json_response({"error": "Session not found"}, status=404)
    return web.json_response({}, status=200)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Mock Xbox WebRTC Streaming Server")
    parser.add_argument("--video", default="/tmp/test_stream.mp4",
                        help="H.264 test video file")
    parser.add_argument("--http-port", type=int, default=8080,
                        help="HTTP API port")
    parser.add_argument("--public-ip", default="127.0.0.1",
                        help="IP address to report in session config (for emulator access)")
    parser.add_argument("--ice-shape", choices=("local", "xbox-teredo"), default="local",
                        help="ICE response shape: local for normal mock streaming, xbox-teredo to mimic real Xbox remote ICE")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--debug-webrtc", action="store_true",
                        help="Enable verbose aiortc/aioice diagnostics")
    parser.add_argument("--debug-sctp", action="store_true",
                        help="Enable focused DTLS/SCTP diagnostics without RTP frame logs")
    parser.add_argument("--send-test-rumble", action="store_true",
                        help="Send one deterministic vibration report after the first valid gamepad packet")
    parser.add_argument("--cloud-titles", type=int, default=80,
                        help="Number of mock xCloud titles to expose for library UI testing")
    args = parser.parse_args()

    global _video_path, _public_ip, _ice_shape, _send_test_rumble
    _video_path = args.video
    _public_ip = args.public_ip
    _ice_shape = args.ice_shape
    _send_test_rumble = args.send_test_rumble

    if args.debug_webrtc or args.debug_sctp:
        install_dtls_certificate_probe()
        install_sctp_probe()
        logging.getLogger("aiortc").setLevel(logging.INFO)
        logging.getLogger("aioice").setLevel(logging.INFO)
        logging.getLogger("aiortc.rtcdtlstransport").setLevel(logging.DEBUG)
        logging.getLogger("aiortc.rtcsctptransport").setLevel(logging.DEBUG)
        logging.getLogger("aiortc.rtcdatachannel").setLevel(logging.DEBUG)
        logging.getLogger("aiortc.rtcrtpsender").setLevel(logging.WARNING)
        logging.getLogger("aiortc.rtcrtpreceiver").setLevel(logging.WARNING)
        logging.getLogger("aiortc.rtp").setLevel(logging.WARNING)
    if args.debug_webrtc:
        logging.getLogger("aioice").setLevel(logging.DEBUG)

    if not os.path.exists(_video_path):
        logger.error(f"Video not found: {_video_path}")
        logger.info("Generate: ffmpeg -f lavfi -i testsrc=duration=120:size=1280x720:rate=30 "
                     "-f lavfi -i sine=frequency=440:duration=120:sample_rate=48000 "
                     "-c:v libx264 -preset ultrafast -tune zerolatency -profile:v baseline "
                     "-pix_fmt yuv420p -g 30 -c:a libopus -b:a 64k -ar 48000 -ac 2 "
                     "-shortest -f mp4 /tmp/test_stream.mp4 -y")
        sys.exit(1)

    logger.info(f"Video: {_video_path}")
    logger.info(f"API: http://{args.host}:{args.http_port}")
    logger.info(f"ICE shape: {_ice_shape}")

    init_cloud_catalog(max_titles=max(0, int(args.cloud_titles)))

    app = web.Application()
    # Home console streaming
    app.router.add_get("/v6/servers/home", handle_consoles)
    app.router.add_post("/v5/sessions/home/play", handle_create_session)
    app.router.add_get("/v5/sessions/home/{sessionId}/state", handle_session_state)
    app.router.add_get("/v5/sessions/home/{sessionId}/configuration", handle_session_config)
    app.router.add_post("/v5/sessions/home/{sessionId}/sdp", handle_sdp_post)
    app.router.add_get("/v5/sessions/home/{sessionId}/sdp", handle_sdp_get)
    app.router.add_post("/v5/sessions/home/{sessionId}/ice", handle_ice_post)
    app.router.add_get("/v5/sessions/home/{sessionId}/ice", handle_ice_get)
    app.router.add_post("/v5/sessions/home/{sessionId}/keepalive", handle_keepalive)
    app.router.add_delete("/v5/sessions/home/{sessionId}", handle_delete_session)

    # Cloud library + cloud session (same host in mock mode)
    app.router.add_get("/v2/titles", handle_cloud_titles)
    app.router.add_get("/v2/titles/mru", handle_cloud_titles_mru)
    app.router.add_post("/v5/sessions/cloud/play", handle_cloud_create_session)
    app.router.add_post("/v5/sessions/cloud/{sessionId}/connect", handle_cloud_connect)
    app.router.add_get("/v5/sessions/cloud/{sessionId}/state", handle_session_state)
    app.router.add_get("/v5/sessions/cloud/{sessionId}/configuration", handle_session_config)
    app.router.add_post("/v5/sessions/cloud/{sessionId}/sdp", handle_sdp_post)
    app.router.add_get("/v5/sessions/cloud/{sessionId}/sdp", handle_sdp_get)
    app.router.add_post("/v5/sessions/cloud/{sessionId}/ice", handle_ice_post)
    app.router.add_get("/v5/sessions/cloud/{sessionId}/ice", handle_ice_get)
    app.router.add_post("/v5/sessions/cloud/{sessionId}/keepalive", handle_keepalive)
    app.router.add_delete("/v5/sessions/cloud/{sessionId}", handle_delete_session)

    # Catalog endpoints used by hydrated library (path aliases on same mock host)
    app.router.add_post("/v3/products", handle_catalog_products)
    app.router.add_post("/catalog/v3/products", handle_catalog_products)
    app.router.add_get("/sigls/v2", handle_new_titles_sigls)
    app.router.add_get("/titles.json", handle_official_titles_json)
    app.router.add_get("/mock/poster/{name}", handle_mock_poster)

    app.router.add_get("/health", lambda r: web.json_response({
        "ok": True,
        "cloudTitles": len(_CLOUD_LIBRARY),
        "recent": len(_CLOUD_RECENT_IDS),
        "new": len(_CLOUD_NEW_IDS),
    }))

    web.run_app(app, host=args.host, port=args.http_port)


if __name__ == "__main__":
    main()
