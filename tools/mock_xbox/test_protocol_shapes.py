#!/usr/bin/env python3
import json
import struct
from pathlib import Path
import unittest

from mock_xbox_server import (
    InputPacketValidator,
    build_test_rumble_packet,
    build_ice_exchange_response,
    build_sdp_exchange_response,
    parse_input_packet,
    parse_remote_ice_candidate,
    preprocess_sdp_offer,
    strip_inline_ice_candidates,
)


class ProtocolShapeTests(unittest.TestCase):
    @staticmethod
    def _metadata_packet(sequence=0):
        return struct.pack("<HIdB", 0x08, sequence, 1.5, 0)

    @staticmethod
    def _gamepad_packet(sequence=1, button_mask=0, left_y=0,
                        left_trigger=0, physicality=0, frame_count=1):
        return struct.pack(
            "<HIdBBHhhhhHHII",
            0x02,
            sequence,
            2.5,
            frame_count,
            0,
            button_mask,
            0,
            left_y,
            0,
            0,
            left_trigger,
            0,
            physicality,
            0,
        )

    def test_input_packet_parser_validates_exact_xstreaming_gamepad_fields(self):
        parsed = parse_input_packet(self._gamepad_packet(
            sequence=7,
            button_mask=0x0010,
            left_y=32767,
            left_trigger=65535,
            physicality=0x000d1000,
        ))

        self.assertEqual(parsed["kind"], "gamepad")
        self.assertEqual(parsed["sequence"], 7)
        self.assertEqual(parsed["button_mask"], 0x0010)
        self.assertEqual(parsed["left_y"], 32767)
        self.assertEqual(parsed["left_trigger"], 65535)
        self.assertEqual(parsed["physicality"], 0x000d1000)
        self.assertEqual(parsed["virtual_physicality"], 0)

    def test_input_packet_validator_tracks_metadata_and_gamepad_sequence(self):
        validator = InputPacketValidator()
        metadata = validator.validate(self._metadata_packet(0))
        gamepad = validator.validate(self._gamepad_packet(1))

        self.assertEqual(metadata["kind"], "client_metadata")
        self.assertEqual(gamepad["sequence"], 1)
        with self.assertRaisesRegex(ValueError, "sequence"):
            validator.validate(self._gamepad_packet(1))

    def test_input_packet_parser_rejects_bad_shape_and_physicality(self):
        with self.assertRaisesRegex(ValueError, "frame count"):
            parse_input_packet(self._gamepad_packet(frame_count=2))
        with self.assertRaisesRegex(ValueError, "physicality"):
            parse_input_packet(self._gamepad_packet(
                button_mask=0x0010,
                physicality=0,
            ))
        with self.assertRaisesRegex(ValueError, "length"):
            parse_input_packet(self._gamepad_packet()[:-1])

    def test_test_rumble_packet_combines_server_metadata_and_vibration(self):
        packet = build_test_rumble_packet()
        self.assertEqual(len(packet), 21)
        self.assertEqual(struct.unpack_from("<HII", packet, 0),
                         (0x90, 720, 1280))
        self.assertEqual(
            packet[10:],
            struct.pack("<BBBBBBHHB", 0, 0, 25, 50, 75, 100, 250, 0, 0),
        )

    def test_sdp_exchange_matches_xstreaming_shape_without_inline_candidates(self):
        raw_sdp = (
            "v=0\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
            "a=mid:0\r\n"
            "a=candidate:1 1 udp 2130706431 192.168.1.2 5000 typ host\r\n"
            "a=end-of-candidates\r\n"
            "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
            "a=mid:2\r\n"
            "a=sctp-port:5000\r\n"
        )

        response = build_sdp_exchange_response(raw_sdp)
        inner = json.loads(response["exchangeResponse"])

        self.assertEqual(inner["status"], "success")
        self.assertEqual(inner["sdpType"], "answer")
        self.assertEqual(inner["messageType"], "answer")
        self.assertEqual(inner["chat"], 1)
        self.assertEqual(inner["control"], 3)
        self.assertEqual(inner["input"], 8)
        self.assertEqual(inner["message"], 1)
        self.assertNotIn("a=candidate:", inner["sdp"])
        self.assertNotIn("a=end-of-candidates", inner["sdp"])
        self.assertIn("a=sctp-port:5000", inner["sdp"])

    def test_ice_exchange_filters_noise_and_appends_end_marker(self):
        candidates = [
            {
                "candidate": "candidate:docker 1 udp 2130706431 172.22.205.2 5000 typ host",
                "sdpMid": "0",
                "sdpMLineIndex": 0,
            },
            {
                "candidate": "candidate:ipv6 1 udp 2130706431 fd00::1 5001 typ host",
                "sdpMid": "0",
                "sdpMLineIndex": 0,
            },
            {
                "candidate": "candidate:lan 1 udp 2130706431 192.168.9.226 5002 typ host",
                "sdpMid": "0",
                "sdpMLineIndex": 0,
            },
        ]

        response = build_ice_exchange_response(candidates, preferred_ip="192.168.9.226")
        inner = json.loads(response["exchangeResponse"])

        self.assertEqual(len(inner), 2)
        self.assertEqual(
            inner[0]["candidate"],
            "a=candidate:1 1 UDP 2130706431 192.168.9.226 5002 typ host",
        )
        self.assertEqual(inner[0]["messageType"], "iceCandidate")
        self.assertEqual(inner[1]["candidate"], "a=end-of-candidates")

    def test_ice_exchange_can_emit_xbox_teredo_shape(self):
        candidates = [
            {
                "candidate": "candidate:lan 1 udp 2130706431 192.168.9.226 5002 typ host",
                "sdpMid": "0",
                "sdpMLineIndex": 0,
            },
        ]

        response = build_ice_exchange_response(
            candidates,
            preferred_ip="192.168.9.226",
            ice_shape="xbox-teredo",
        )
        inner = json.loads(response["exchangeResponse"])

        self.assertEqual(
            inner[0]["candidate"],
            "a=candidate:1 1 UDP 100 192.168.1.10 9002 typ host ",
        )
        self.assertIn(" 2001:db8:1::11 ", inner[1]["candidate"])
        self.assertEqual(inner[2]["sdpMid"], "0")
        self.assertEqual(inner[3]["candidate"], "a=end-of-candidates")

        teredo_ip = inner[2]["candidate"].split()[4]
        self.assertTrue(teredo_ip.startswith("2001:"))
        words = teredo_ip.split(":")
        self.assertEqual(len(words), 8)
        mapped_port = (~int(words[5], 16)) & 0xffff
        client4 = (~int(words[6] + words[7], 16)) & 0xffffffff
        self.assertEqual(mapped_port, 5002)
        self.assertEqual(
            ".".join(str((client4 >> shift) & 0xff) for shift in (24, 16, 8, 0)),
            "192.168.9.226",
        )

    def test_strip_inline_ice_candidates_preserves_non_candidate_lines(self):
        raw_sdp = "v=0\r\na=ice-ufrag:test\r\na=candidate:1 1 udp 1 1.1.1.1 1 typ host\r\na=setup:active\r\n"
        self.assertEqual(
            strip_inline_ice_candidates(raw_sdp),
            "v=0\r\na=ice-ufrag:test\r\na=setup:passive\r\n",
        )

    def test_sdp_exchange_keeps_only_sha256_fingerprint_for_libpeer(self):
        raw_sdp = (
            "v=0\r\n"
            "a=fingerprint:sha-256 AA:BB\r\n"
            "a=fingerprint:sha-384 CC:DD\r\n"
            "a=fingerprint:sha-512 EE:FF\r\n"
        )

        response = build_sdp_exchange_response(raw_sdp)
        inner = json.loads(response["exchangeResponse"])

        self.assertIn("a=fingerprint:sha-256 AA:BB", inner["sdp"])
        self.assertNotIn("a=fingerprint:sha-384", inner["sdp"])
        self.assertNotIn("a=fingerprint:sha-512", inner["sdp"])

    def test_sdp_exchange_forces_xbox_answer_dtls_role_passive(self):
        raw_sdp = (
            "v=0\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
            "a=setup:passive\r\n"
            "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
            "a=setup:actpass\r\n"
        )

        response = build_sdp_exchange_response(raw_sdp)
        inner = json.loads(response["exchangeResponse"])

        self.assertNotIn("a=setup:active", inner["sdp"])
        self.assertNotIn("a=setup:actpass", inner["sdp"])
        self.assertEqual(inner["sdp"].count("a=setup:passive"), 2)

    def test_preprocess_lunarnx_offer_rewrites_video_to_recvonly_only(self):
        raw_sdp = (
            "v=0\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
            "a=fmtp:96 profile-level-id=42e01f;level-asymmetry-allowed=1\r\n"
            "a=sendrecv\r\n"
            "m=audio 9 UDP/TLS/RTP/SAVP 111\r\n"
            "a=sendrecv\r\n"
            "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
        )

        processed = preprocess_sdp_offer(raw_sdp)
        video_section = processed.split("m=audio", 1)[0]
        audio_section = "m=audio" + processed.split("m=audio", 1)[1].split("m=application", 1)[0]

        self.assertIn(
            "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
            video_section,
        )
        self.assertIn("a=recvonly", video_section)
        self.assertNotIn("a=sendrecv", video_section)
        self.assertIn("a=sendrecv", audio_section)

    def test_parse_remote_ice_candidate_uses_aiortc_shape(self):
        candidate = parse_remote_ice_candidate({
            "candidate": "a=candidate:1 1 UDP 2130706431 192.168.9.10 6000 typ host",
            "sdpMid": "0",
            "sdpMLineIndex": 0,
        })

        self.assertEqual(candidate.foundation, "1")
        self.assertEqual(candidate.component, 1)
        self.assertEqual(candidate.protocol, "UDP")
        self.assertEqual(candidate.ip, "192.168.9.10")
        self.assertEqual(candidate.port, 6000)
        self.assertEqual(candidate.sdpMid, "0")
        self.assertEqual(candidate.sdpMLineIndex, 0)

    def test_video_track_handles_pyav_eof_explicitly(self):
        source = Path(__file__).with_name("mock_xbox_server.py").read_text()

        self.assertIn("av.error.EOFError", source)
        self.assertIn("_next_decoded_video_frame", source)

    def test_media_tracks_are_paced_by_timestamps(self):
        source = Path(__file__).with_name("mock_xbox_server.py").read_text()

        self.assertIn("async def _pace_media", source)
        self.assertIn("await _pace_media(self._pace_start, float(frame.pts * frame.time_base))", source)
        self.assertIn("await _pace_media(self._pace_start, float(rf.pts * rf.time_base))", source)

    def test_looping_media_keeps_rtp_timestamps_monotonic(self):
        source = Path(__file__).with_name("mock_xbox_server.py").read_text()
        video_restart = source.split("def _restart_video", 1)[1].split(
            "async def recv", 1)[0]
        audio_loop = source.split("class AudioFileTrack", 1)[1].split(
            "# ---------------------------------------------------------------------------\n"
            "# WebRTC session", 1)[0].split("# Loop", 1)[1]

        self.assertNotIn("self._frame_count = 0", video_restart)
        self.assertNotIn("self._samples = 0", audio_loop)

    def test_control_keyframe_request_forces_aiortc_keyframe(self):
        source = Path(__file__).with_name("mock_xbox_server.py").read_text()

        self.assertIn("_setup_control_channel(channel, video_sender)", source)
        self.assertIn("video_sender._send_keyframe()", source)


if __name__ == "__main__":
    unittest.main()
