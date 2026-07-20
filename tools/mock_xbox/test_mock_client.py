#!/usr/bin/env python3
"""Test client for mock Xbox WebRTC server. Matches XStreaming's protocol."""

import argparse
import asyncio
import json
import logging
import sys
import time
import requests
from aiortc import (
    RTCPeerConnection,
    RTCSessionDescription,
    RTCConfiguration,
    RTCIceServer,
)
from aiortc.sdp import candidate_from_sdp

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("test-client")


def _extract_inline_candidates(sdp: str):
    candidates = []
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
        if line.startswith("a=candidate:"):
            candidates.append({
                "candidate": line[len("a="):],
                "sdpMid": current_mid,
                "sdpMLineIndex": max(current_mline, 0),
            })

    return candidates


async def test_mock_server(base_url: str):
    logger.info(f"Testing mock server at {base_url}")

    # Step 1: Console list
    resp = requests.get(f"{base_url}/v6/servers/home")
    resp.raise_for_status()
    server_id = resp.json()["results"][0]["serverId"]
    logger.info(f"Console: {server_id}")

    # Step 2: Create session
    resp = requests.post(f"{base_url}/v5/sessions/home/play", json={
        "serverId": server_id,
        "settings": {
            "nanoVersion": "V3;WebrtcTransport.dll",
            "locale": "en-US",
            "osName": "windows",
        },
    })
    resp.raise_for_status()
    session_id = resp.json()["sessionId"]
    logger.info(f"Session: {session_id}")

    # Step 3: Wait for Provisioned
    for i in range(30):
        resp = requests.get(f"{base_url}/v5/sessions/home/{session_id}/state")
        st = resp.json().get("state", "")
        if st == "Provisioned":
            break
        await asyncio.sleep(1)
    logger.info(f"State: {st}")

    # Step 4: Get config
    resp = requests.get(f"{base_url}/v5/sessions/home/{session_id}/configuration")
    resp.raise_for_status()
    logger.info(f"Config: ip={resp.json()['serverDetails']['ipV4Address']}")

    # Step 5: Create PeerConnection (XStreaming style)
    pc = RTCPeerConnection(configuration=RTCConfiguration(
        iceServers=[RTCIceServer(urls="stun:stun.l.google.com:19302")]
    ))

    # XStreaming: addTransceiver video(recvonly), audio(sendrecv)
    pc.addTransceiver("video", direction="recvonly")
    pc.addTransceiver("audio", direction="sendrecv")

    # XStreaming: non-negotiated data channels with protocols
    # input: ordered=true, protocol='1.0'
    # chat: protocol='chatV1'
    # control: protocol='controlV1'
    # message: protocol='messageV1'
    chat_ch = pc.createDataChannel("chat", protocol="chatV1")
    ctrl_ch = pc.createDataChannel("control", protocol="controlV1")
    input_ch = pc.createDataChannel("input", ordered=True, protocol="1.0")
    msg_ch = pc.createDataChannel("message", protocol="messageV1")

    video_frames = 0
    audio_frames = 0
    handshake_ack = asyncio.Event()

    @pc.on("track")
    def on_track(track):
        logger.info(f"Track: {track.kind}")
        async def read_track(kind):
            nonlocal video_frames, audio_frames
            try:
                while True:
                    frame = await track.recv()
                    if kind == "video":
                        video_frames += 1
                        if video_frames % 60 == 0:
                            logger.info(f"Video: {video_frames} frames")
                    else:
                        audio_frames += 1
            except Exception as e:
                logger.info(f"{kind} track ended: {e}")
        asyncio.create_task(read_track(track.kind))

    @pc.on("iceconnectionstatechange")
    async def _():
        logger.info(f"ICE: {pc.iceConnectionState}")

    @pc.on("connectionstatechange")
    async def _():
        logger.info(f"Conn: {pc.connectionState}")

    @msg_ch.on("message")
    def on_msg(message):
        text = message if isinstance(message, str) else message.decode()
        logger.info(f"[MSG] {text[:150]}")
        try:
            data = json.loads(text)
            if data.get("type") == "HandshakeAck":
                logger.info("HandshakeAck received!")
                handshake_ack.set()
                if ctrl_ch.readyState == "open":
                    ctrl_ch.send(json.dumps({
                        "message": "authorizationRequest",
                        "accessKey": "4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E",
                    }))
                    logger.info("Sent authorizationRequest")
        except json.JSONDecodeError:
            pass

    @ctrl_ch.on("open")
    def _(): logger.info("[CTRL] open")

    @input_ch.on("open")
    def _(): logger.info("[INPUT] open")

    local_ice = []

    @pc.on("icecandidate")
    def on_ice(c):
        if c and c.candidate:
            cand_raw = c.candidate
            if not cand_raw.startswith("candidate:"):
                cand_raw = f"candidate:{cand_raw}"
            local_ice.append({
                "candidate": cand_raw,
                "sdpMid": c.sdpMid or "0",
                "sdpMLineIndex": c.sdpMLineIndex if c.sdpMLineIndex is not None else 0,
            })

    # Create and send offer
    offer = await pc.createOffer()
    await pc.setLocalDescription(offer)
    local_ice.extend(_extract_inline_candidates(pc.localDescription.sdp))

    sdp_body = {
        "messageType": "offer",
        "sdp": offer.sdp,
        "configuration": {
            "chatConfiguration": {
                "bytesPerSample": 2,
                "expectedClipDurationMs": 20,
                "format": {"codec": "opus", "container": "webm"},
                "numChannels": 1,
                "sampleFrequencyHz": 24000,
            },
            "chat": {"minVersion": 1, "maxVersion": 1},
            "control": {"minVersion": 1, "maxVersion": 3},
            "input": {"minVersion": 1, "maxVersion": 8},
            "message": {"minVersion": 1, "maxVersion": 1},
        },
    }

    resp = requests.post(f"{base_url}/v5/sessions/home/{session_id}/sdp",
                         json=sdp_body)
    if resp.status_code != 200:
        logger.error(f"SDP post failed: {resp.status_code} {resp.text}")
        return
    logger.info("SDP offer sent")

    # Poll for answer
    answer_sdp = ""
    for i in range(60):
        await asyncio.sleep(0.1)
        resp = requests.get(f"{base_url}/v5/sessions/home/{session_id}/sdp")
        data = resp.json()
        exchange = data.get("exchangeResponse", "")
        if exchange:
            inner = json.loads(exchange)
            answer_sdp = inner.get("sdp", "")
            if answer_sdp:
                break

    if not answer_sdp:
        logger.error("No SDP answer")
        return

    await pc.setRemoteDescription(RTCSessionDescription(sdp=answer_sdp, type="answer"))
    logger.info("Remote answer set")

    # Wait for ICE gathering
    await asyncio.sleep(0.5)

    # Exchange ICE
    if local_ice:
        resp = requests.post(f"{base_url}/v5/sessions/home/{session_id}/ice",
                             json={"iceCandidates": local_ice})
        logger.info(f"Sent {len(local_ice)} ICE candidates")

    await asyncio.sleep(0.5)
    resp = requests.get(f"{base_url}/v5/sessions/home/{session_id}/ice")
    data = resp.json()
    exchange = data.get("exchangeResponse", "")
    if exchange:
        remote = json.loads(exchange)
        for c in remote:
            cand_str = c.get("candidate", "")
            if cand_str.startswith("a=candidate:"):
                cand_str = cand_str[len("a=candidate:"):]
            elif cand_str.startswith("candidate:"):
                cand_str = cand_str[len("candidate:"):]
            try:
                candidate = candidate_from_sdp(cand_str)
                candidate.sdpMid = str(c.get("sdpMid", "0") or "0")
                candidate.sdpMLineIndex = int(c.get("sdpMLineIndex", 0) or 0)
                await pc.addIceCandidate(candidate)
            except Exception as e:
                logger.warning(f"Failed to add remote candidate: {e}")
        logger.info(f"Added {len(remote)} remote ICE candidates")

    # Wait for connection
    logger.info("Waiting for connection...")
    for _ in range(100):
        if pc.connectionState == "connected" and msg_ch.readyState == "open":
            break
        await asyncio.sleep(0.1)

    if pc.connectionState != "connected" or msg_ch.readyState != "open":
        logger.error(
            f"Connection failed: connectionState={pc.connectionState} "
            f"messageChannel={msg_ch.readyState}"
        )
        await pc.close()
        return False

    # Send messageV1 handshake (XStreaming style)
    msg_ch.send(json.dumps({
        "type": "Handshake",
        "version": "messageV1",
        "id": "test-001",
        "cv": "test.1",
    }))
    logger.info("Sent Handshake")

    try:
        await asyncio.wait_for(handshake_ack.wait(), timeout=5.0)
    except asyncio.TimeoutError:
        logger.error("Timed out waiting for HandshakeAck")
        await pc.close()
        return False

    # Stream for a while
    logger.info("Streaming (5s)...")
    await asyncio.sleep(5)

    logger.info(f"Results: video={video_frames} audio={audio_frames}")
    await pc.close()
    logger.info("Done")
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default="http://localhost:8080")
    args = parser.parse_args()
    ok = asyncio.run(test_mock_server(args.server))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
