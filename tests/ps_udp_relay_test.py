#!/usr/bin/env python3
import hashlib
import hmac
import http.server
import importlib.util
import json
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RELAY = ROOT / "tools/ps_udp_relay/ps_udp_relay.py"
CHIAKI_PATCH = ROOT / "tools/chiaki_switch/lunarnx-chiaki-switch.patch"
MAGIC_COOKIE = 0x2112A442
PROTOCOL_VERSION = 3
HMAC_LABEL = b"LUNARNX-PS-UDP-RELAY-V3"
RUDP_MAGIC = 0x244F244F

RELAY_SPEC = importlib.util.spec_from_file_location("ps_udp_relay", RELAY)
RELAY_MODULE = importlib.util.module_from_spec(RELAY_SPEC)
RELAY_SPEC.loader.exec_module(RELAY_MODULE)


def control_hmac(secret, direction, request_id, nonce, payload):
    message = b"\0".join((
        HMAC_LABEL,
        direction.encode(),
        request_id.encode(),
        nonce.encode(),
        payload.encode(),
    ))
    return hmac.new(secret.encode(), message, hashlib.sha256).hexdigest()


class RelayControl:
    def __init__(self, address, secret):
        self.secret = secret
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(2)
        self.sock.connect(address)
        self.nonce = None
        self.connection_id = secrets.token_hex(16)

    def close(self):
        self.sock.close()

    def authenticate(self):
        request_id = secrets.token_hex(16)
        payload = json.dumps({"cmd": "challenge"}, separators=(",", ":"))
        packet = json.dumps({
            "version": PROTOCOL_VERSION,
            "request_id": request_id,
            "payload": payload,
        }, separators=(",", ":")).encode()
        self.sock.send(packet)
        envelope = json.loads(self.sock.recv(65535))
        self.assert_envelope(envelope, request_id, allow_new_nonce=True)
        self.nonce = envelope["nonce"]
        response = json.loads(envelope["payload"])
        if not response.get("ok"):
            raise RuntimeError("relay challenge failed")

    def encode(self, value, request_id=None, hmac_override=None):
        value = dict(value)
        if value.get("cmd") in {
                "open", "peers", "status", "close", "close_connection"}:
            value.setdefault("connection_id", self.connection_id)
        request_id = request_id or secrets.token_hex(16)
        payload = json.dumps(value, separators=(",", ":"))
        digest = control_hmac(
            self.secret, "request", request_id, self.nonce, payload)
        envelope = {
            "version": PROTOCOL_VERSION,
            "request_id": request_id,
            "nonce": self.nonce,
            "payload": payload,
            "hmac": hmac_override or digest,
        }
        return request_id, json.dumps(envelope, separators=(",", ":")).encode()

    def receive(self, request_id):
        raw = self.sock.recv(65535)
        envelope = json.loads(raw)
        self.assert_envelope(envelope, request_id)
        return json.loads(envelope["payload"]), raw

    def request(self, value, request_id=None):
        request_id, packet = self.encode(value, request_id=request_id)
        self.sock.send(packet)
        response, _ = self.receive(request_id)
        return response

    def assert_envelope(self, envelope, request_id, allow_new_nonce=False):
        if envelope.get("version") != PROTOCOL_VERSION:
            raise RuntimeError("invalid relay response version")
        if envelope.get("request_id") != request_id:
            raise RuntimeError("relay response request ID mismatch")
        nonce = envelope.get("nonce")
        if not isinstance(nonce, str) or len(nonce) != 64:
            raise RuntimeError("invalid relay nonce")
        if not allow_new_nonce and nonce != self.nonce:
            raise RuntimeError("relay response nonce mismatch")
        payload = envelope.get("payload")
        digest = envelope.get("hmac")
        expected = control_hmac(
            self.secret, "response", request_id, nonce, payload)
        if not hmac.compare_digest(str(digest), expected):
            raise RuntimeError("relay response HMAC mismatch")


class FakeStun(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.seen_sources = []
        self.running = True

    def run(self):
        while self.running:
            try:
                packet, source = self.sock.recvfrom(2048)
            except OSError:
                return
            if len(packet) < 20 or packet[:2] != b"\x00\x01":
                continue
            self.seen_sources.append(source)
            transaction = packet[8:20]
            ip = socket.inet_aton(source[0])
            xport = source[1] ^ (MAGIC_COOKIE >> 16)
            xip = bytes(a ^ b for a, b in zip(ip, struct.pack("!I", MAGIC_COOKIE)))
            attribute = struct.pack("!HHBBH", 0x0020, 8, 0, 1, xport) + xip
            response = (b"\x01\x01" + struct.pack("!H", len(attribute)) +
                        struct.pack("!I", MAGIC_COOKIE) + transaction + attribute)
            self.sock.sendto(response, source)

    def close(self):
        self.running = False
        self.sock.close()


class FakePlayStation(threading.Thread):
    def __init__(self, marker):
        super().__init__(daemon=True)
        self.marker = marker
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.sources = []
        self.running = True

    def run(self):
        while self.running:
            try:
                packet, source = self.sock.recvfrom(65535)
            except OSError:
                return
            self.sources.append(source)
            self.sock.sendto(b"not-a-holepunch-packet", source)
            response = bytearray(packet)
            response[:4] = struct.pack("!I", 0x07000000)
            response[4:5] = self.marker
            self.sock.sendto(response, source)

    def close(self):
        self.running = False
        self.sock.close()


class PortRewritingPlayStation(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.candidate_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.candidate_sock.bind(("127.0.0.1", 0))
        self.port = self.candidate_sock.getsockname()[1]
        self.response_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.response_sock.bind(("127.0.0.1", 0))
        self.response_port = self.response_sock.getsockname()[1]
        self.running = True

    def run(self):
        while self.running:
            try:
                packet, source = self.candidate_sock.recvfrom(65535)
            except OSError:
                return
            if len(packet) != 88:
                continue
            response = bytearray(packet)
            response[:4] = struct.pack("!I", 0x07000000)
            self.response_sock.sendto(response, source)

    def close(self):
        self.running = False
        self.candidate_sock.close()
        self.response_sock.close()


def rudp_packet(payload=b"", packet_type=0):
    size = 8 + len(payload)
    return struct.pack("!HIH", size, RUDP_MAGIC, packet_type) + payload


class MigratingRudpPlayStation(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.candidate_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.candidate_sock.bind(("127.0.0.1", 0))
        self.port = self.candidate_sock.getsockname()[1]
        self.migrated_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.migrated_sock.bind(("127.0.0.1", 0))
        self.migrated_port = self.migrated_sock.getsockname()[1]
        self.noise_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.noise_sock.bind(("127.0.0.1", 0))
        self.relay_source = None
        self.holepunch_response = None
        self.rudp_request_seen = threading.Event()
        self.holepunch_response_seen = threading.Event()
        self.running = True

    def run(self):
        while self.running:
            try:
                packet, source = self.candidate_sock.recvfrom(65535)
            except OSError:
                return
            self.relay_source = source
            if len(packet) == 88:
                response = bytearray(packet)
                response[:4] = struct.pack("!I", 0x07000000)
                self.holepunch_response = bytes(response)
                self.candidate_sock.sendto(self.holepunch_response, source)
                self.holepunch_response_seen.set()
            elif len(packet) >= 8 and packet[2:6] == struct.pack("!I", RUDP_MAGIC):
                self.rudp_request_seen.set()

    def send_migrated(self, packet):
        if not self.rudp_request_seen.wait(timeout=2):
            raise RuntimeError("relay did not send RUDP request to selected peer")
        self.migrated_sock.sendto(packet, self.relay_source)

    def send_duplicate_holepunch(self):
        if not self.holepunch_response_seen.wait(timeout=2):
            raise RuntimeError("relay did not send holepunch request")
        self.candidate_sock.sendto(self.holepunch_response, self.relay_source)

    def send_noise(self, packet):
        self.noise_sock.sendto(packet, self.relay_source)

    def close(self):
        self.running = False
        self.candidate_sock.close()
        self.migrated_sock.close()
        self.noise_sock.close()


class NoisyStun(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.running = True

    def run(self):
        try:
            packet, source = self.sock.recvfrom(2048)
            if len(packet) < 20:
                return
            while self.running:
                # Keep traffic flowing from the expected server, but never use
                # the request transaction ID.
                response = (b"\x01\x01\x00\x00" +
                            struct.pack("!I", MAGIC_COOKIE) + b"N" * 12)
                self.sock.sendto(response, source)
                time.sleep(0.01)
        except OSError:
            return

    def close(self):
        self.running = False
        self.sock.close()


class FakePsnSessionMessageServer:
    def __init__(self):
        self.requests = []
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                size = int(self.headers.get("Content-Length", "0"))
                owner.requests.append({
                    "path": self.path,
                    "authorization": self.headers.get("Authorization"),
                    "content_type": self.headers.get("Content-Type"),
                    "body": self.rfile.read(size),
                })
                self.send_response(204)
                self.end_headers()

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(
            target=self.server.serve_forever, daemon=True)

    @property
    def url_template(self):
        return (f"http://127.0.0.1:{self.server.server_port}/api/"
                "sessionManager/v1/remotePlaySessions/{session_id}/"
                "sessionMessage")

    def start(self):
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=1)


class RelayTest(unittest.TestCase):
    def test_psn_session_message_post_uses_local_token_and_exact_body(self):
        server = FakePsnSessionMessageServer()
        server.start()
        try:
            with tempfile.TemporaryDirectory() as temp:
                token_file = Path(temp) / "psn_token.json"
                token_file.write_text(json.dumps({
                    "access_token": "local-mac-token",
                }))
                session_id = "12345678-1234-1234-1234-123456789abc"
                body = b'{"message":"exact-body"}'
                status = RELAY_MODULE.post_psn_session_message(
                    token_file, session_id, body,
                    url_template=server.url_template)

            self.assertEqual(status, 204)
            self.assertEqual(len(server.requests), 1)
            request = server.requests[0]
            self.assertEqual(
                request["path"],
                f"/api/sessionManager/v1/remotePlaySessions/{session_id}/"
                "sessionMessage")
            self.assertEqual(request["authorization"],
                             "Bearer local-mac-token")
            self.assertEqual(request["content_type"],
                             "application/json; charset=utf-8")
            self.assertEqual(request["body"], body)
        finally:
            server.close()

    def test_playstation_holepunch_retries_are_coalesced_for_slow_guest(self):
        stun = FakeStun()
        stun.start()
        peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        peer.bind(("127.0.0.1", 0))
        peer.settimeout(0.5)
        guest = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        guest.bind(("127.0.0.1", 0))
        guest.settimeout(0.2)
        channel = None
        try:
            channel = RELAY_MODULE.RelayChannel(
                "slow-guest-test", "ctrl", "127.0.0.1",
                ("127.0.0.1", stun.port), 30, False)
            channel.set_peers([{
                "host": "127.0.0.1", "port": peer.getsockname()[1],
            }])

            guest_request = bytearray(88)
            guest_request[:4] = struct.pack("!I", 0x06000000)
            guest_request[0x4b:0x50] = b"guest"
            guest.sendto(guest_request,
                         ("127.0.0.1", channel.tunnel_port))
            forwarded_request, relay_source = peer.recvfrom(88)
            self.assertEqual(forwarded_request, guest_request)

            peer_response = bytearray(guest_request)
            peer_response[:4] = struct.pack("!I", 0x07000000)
            peer.sendto(peer_response, relay_source)
            self.assertEqual(guest.recv(88), peer_response)

            ps_requests = []
            for index in range(6):
                request = bytearray(88)
                request[:4] = struct.pack("!I", 0x06000000)
                request[0x4b:0x50] = index.to_bytes(5, "big")
                ps_requests.append(bytes(request))
                peer.sendto(request, relay_source)

            first_request = guest.recv(88)
            self.assertEqual(first_request, ps_requests[0])

            accelerated_response = peer.recvfrom(88)[0]
            self.assertEqual(
                struct.unpack("!I", accelerated_response[:4])[0],
                0x07000000)
            self.assertEqual(accelerated_response[4:0x48],
                             guest_request[4:0x48])
            self.assertEqual(accelerated_response[0x4b:0x50],
                             first_request[0x4b:0x50])
            expected_address = socket.inet_aton("127.0.0.1")
            expected_port = struct.pack("!H", peer.getsockname()[1])
            self.assertEqual(
                accelerated_response[0x50:0x54],
                bytes(a ^ b for a, b in zip(
                    guest_request[0x44:0x48], expected_address)))
            self.assertEqual(
                accelerated_response[0x54:0x56],
                bytes(a ^ b for a, b in zip(
                    guest_request[0x44:0x46], expected_port)))
            with self.assertRaises(socket.timeout):
                guest.recv(88)

            guest_response = bytearray(first_request)
            guest_response[:4] = struct.pack("!I", 0x07000000)
            guest.sendto(guest_response,
                         ("127.0.0.1", channel.tunnel_port))
            self.assertEqual(peer.recvfrom(88)[0], guest_response)

            time.sleep(0.05)
            with self.assertRaises(socket.timeout):
                guest.recv(88)
            self.assertEqual(
                channel.status()["holepunch_retries_coalesced"], 5)
            self.assertEqual(
                channel.status()["holepunch_responses_accelerated"], 1)
        finally:
            if channel:
                channel.close()
            guest.close()
            peer.close()
            stun.close()

    def test_early_playstation_holepunch_is_delivered_when_guest_arrives(self):
        stun = FakeStun()
        stun.start()
        peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        peer.bind(("127.0.0.1", 0))
        guest = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        guest.bind(("127.0.0.1", 0))
        guest.settimeout(0.5)
        channel = None
        try:
            channel = RELAY_MODULE.RelayChannel(
                "early-packet-test", "ctrl", "127.0.0.1",
                ("127.0.0.1", stun.port), 30, False)
            early = bytearray(88)
            early[:4] = struct.pack("!I", 0x06000000)
            early[0x4b:0x50] = b"early"
            peer.sendto(early, channel.mapped)
            time.sleep(0.05)

            channel.set_peers([{
                "host": "127.0.0.1", "port": peer.getsockname()[1],
            }])
            request = bytearray(88)
            request[:4] = struct.pack("!I", 0x06000000)
            request[0x4b:0x50] = b"guest"
            guest.sendto(request, ("127.0.0.1", channel.tunnel_port))

            self.assertEqual(guest.recv(88), early)
            self.assertEqual(channel.status()["selected_peer"],
                             f"127.0.0.1:{peer.getsockname()[1]}")
        finally:
            if channel:
                channel.close()
            guest.close()
            peer.close()
            stun.close()

    def test_switch_client_uses_retriable_nonblocking_udp_control(self):
        patch = CHIAKI_PATCH.read_text()
        self.assertIn(
            "+    chiaki_socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);",
            patch)
        self.assertIn("+#define RELAY_CONTROL_RETRY_COUNT 6", patch)
        self.assertIn("+    if(chiaki_socket_set_nonblock(sock, true)", patch)
        self.assertIn("+    char relay_nonce[65];", patch)
        self.assertNotIn(
            "+    chiaki_socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);",
            patch)

    def test_stun_hostname_is_normalized_to_response_source(self):
        stun = FakeStun()
        stun.start()
        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client.bind(("127.0.0.1", 0))
        try:
            mapped = RELAY_MODULE.stun_mapping(
                client, ("localhost", stun.port), timeout=0.5)
            self.assertEqual(mapped, client.getsockname())
        finally:
            client.close()
            stun.close()

    def test_stun_timeout_is_a_total_deadline(self):
        stun = NoisyStun()
        stun.start()
        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client.bind(("127.0.0.1", 0))
        result = {}

        def query():
            try:
                RELAY_MODULE.stun_mapping(
                    client, ("127.0.0.1", stun.port), timeout=0.1)
            except Exception as error:
                result["error"] = error

        worker = threading.Thread(target=query, daemon=True)
        worker.start()
        worker.join(timeout=0.35)
        completed_before_deadline = not worker.is_alive()
        stun.close()
        client.close()
        worker.join(timeout=0.5)

        self.assertTrue(completed_before_deadline,
                        "unrelated UDP traffic must not reset the STUN deadline")
        self.assertIsInstance(result.get("error"), socket.timeout)

    def test_authenticated_ctrl_and_data_forwarding(self):
        self.assertTrue(RELAY.exists(), "relay helper must exist")
        stun = FakeStun()
        ctrl_peer = FakePlayStation(b"C")
        data_peer = FakePlayStation(b"D")
        stun.start()
        ctrl_peer.start()
        data_peer.start()
        with tempfile.TemporaryDirectory() as temp:
            ready = Path(temp) / "ready.json"
            process = subprocess.Popen([
                sys.executable, str(RELAY),
                "--listen-address", "127.0.0.1",
                "--control-port", "0",
                "--secret", "relay-test-secret",
                "--stun-server", f"127.0.0.1:{stun.port}",
                "--ready-file", str(ready),
                "--idle-timeout", "30",
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    if process.poll() is not None:
                        stdout, stderr = process.communicate()
                        self.fail(f"relay exited early: {stdout}\n{stderr}")
                    time.sleep(0.02)
                info = json.loads(ready.read_text())
                control = RelayControl(
                    ("127.0.0.1", info["control_port"]), "relay-test-secret")
                control.authenticate()

                channels = {}
                for name, peer in (("ctrl", ctrl_peer), ("data", data_peer)):
                    if name == "ctrl":
                        request_id, packet = control.encode(
                            {"cmd": "open", "channel": name})
                        control.sock.send(packet)
                        control.sock.send(packet)
                        opened, first_raw = control.receive(request_id)
                        repeated, second_raw = control.receive(request_id)
                        self.assertEqual(opened, repeated)
                        self.assertEqual(first_raw, second_raw)
                        self.assertEqual(len(stun.seen_sources), 1,
                                         "retry must not replace the UDP mapping")
                    else:
                        opened = control.request({"cmd": "open", "channel": name})
                    self.assertTrue(opened["ok"])
                    self.assertEqual(opened["mapped_host"], "127.0.0.1")
                    self.assertGreater(opened["mapped_port"], 0)
                    self.assertTrue(control.request({
                        "cmd": "peers", "channel": name,
                        "peers": [{"host": "127.0.0.1", "port": peer.port}],
                    })["ok"])
                    channels[name] = opened

                ctrl_guest = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                ctrl_guest.settimeout(2)
                data_guest = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                data_guest.settimeout(2)
                ctrl_probe = bytearray(b"Q" * 88)
                ctrl_probe[:4] = struct.pack("!I", 0x06000000)
                data_probe = bytearray(b"R" * 88)
                data_probe[:4] = struct.pack("!I", 0x06000000)
                ctrl_guest.sendto(ctrl_probe, ("127.0.0.1", channels["ctrl"]["tunnel_port"]))
                data_guest.sendto(data_probe, ("127.0.0.1", channels["data"]["tunnel_port"]))
                ctrl_response = ctrl_guest.recv(88)
                data_response = data_guest.recv(88)
                self.assertEqual(struct.unpack("!I", ctrl_response[:4])[0], 0x07000000)
                self.assertEqual(struct.unpack("!I", data_response[:4])[0], 0x07000000)
                self.assertEqual(ctrl_response[4:5], b"C")
                self.assertEqual(data_response[4:5], b"D")
                self.assertEqual(ctrl_response[5:], ctrl_probe[5:])
                self.assertEqual(data_response[5:], data_probe[5:])

                self.assertEqual(ctrl_peer.sources[0][1], channels["ctrl"]["mapped_port"])
                self.assertEqual(data_peer.sources[0][1], channels["data"]["mapped_port"])
                self.assertNotEqual(channels["ctrl"]["mapped_port"], channels["data"]["mapped_port"])

                status = control.request({"cmd": "status"})
                self.assertEqual(status["channels"]["ctrl"]["selected_peer"],
                                 f"127.0.0.1:{ctrl_peer.port}")
                self.assertEqual(status["channels"]["data"]["selected_peer"],
                                 f"127.0.0.1:{data_peer.port}")
                self.assertTrue(control.request({"cmd": "shutdown"})["ok"])
                control.close()
                self.assertEqual(process.wait(timeout=3), 0)
                process.communicate()
                ctrl_guest.close()
                data_guest.close()
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=3)
                stun.close()
                ctrl_peer.close()
                data_peer.close()

    def test_accepts_valid_holepunch_response_from_rewritten_peer_port(self):
        stun = FakeStun()
        peer = PortRewritingPlayStation()
        stun.start()
        peer.start()
        with tempfile.TemporaryDirectory() as temp:
            ready = Path(temp) / "ready.json"
            process = subprocess.Popen([
                sys.executable, str(RELAY),
                "--listen-address", "127.0.0.1",
                "--control-port", "0",
                "--secret", "relay-test-secret",
                "--stun-server", f"127.0.0.1:{stun.port}",
                "--ready-file", str(ready),
                "--idle-timeout", "30",
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            guest = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            guest.settimeout(2)
            control = None
            try:
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                info = json.loads(ready.read_text())
                control = RelayControl(
                    ("127.0.0.1", info["control_port"]), "relay-test-secret")
                control.authenticate()
                opened = control.request({"cmd": "open", "channel": "ctrl"})
                self.assertTrue(control.request({
                    "cmd": "peers", "channel": "ctrl",
                    "peers": [{"host": "127.0.0.1", "port": peer.port}],
                })["ok"])
                request = bytearray(88)
                request[:4] = struct.pack("!I", 0x06000000)
                request[0x4b:0x50] = b"abcde"
                guest.sendto(request, ("127.0.0.1", opened["tunnel_port"]))
                response = guest.recv(88)
                self.assertEqual(struct.unpack("!I", response[:4])[0], 0x07000000)
                self.assertEqual(response[0x4b:0x50], b"abcde")
                status = control.request({"cmd": "status"})
                self.assertEqual(status["channels"]["ctrl"]["selected_peer"],
                                 f"127.0.0.1:{peer.response_port}")
                self.assertTrue(control.request({"cmd": "shutdown"})["ok"])
            finally:
                guest.close()
                if control:
                    control.close()
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=3)
                process.communicate()
                stun.close()
                peer.close()

    def test_connections_do_not_replace_each_others_channels(self):
        stun = FakeStun()
        stun.start()
        with tempfile.TemporaryDirectory() as temp:
            ready = Path(temp) / "ready.json"
            process = subprocess.Popen([
                sys.executable, str(RELAY),
                "--listen-address", "127.0.0.1",
                "--control-port", "0",
                "--secret", "relay-test-secret",
                "--stun-server", f"127.0.0.1:{stun.port}",
                "--ready-file", str(ready),
                "--idle-timeout", "30",
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            first = second = None
            try:
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                info = json.loads(ready.read_text())
                address = ("127.0.0.1", info["control_port"])
                first = RelayControl(address, "relay-test-secret")
                second = RelayControl(address, "relay-test-secret")
                first.authenticate()
                second.authenticate()
                first_open = first.request({"cmd": "open", "channel": "ctrl"})
                second_open = second.request({"cmd": "open", "channel": "ctrl"})
                self.assertNotEqual(first.connection_id, second.connection_id)
                self.assertNotEqual(first_open["mapped_port"], second_open["mapped_port"])
                self.assertIn("ctrl", first.request({"cmd": "status"})["channels"])
                self.assertIn("ctrl", second.request({"cmd": "status"})["channels"])
                self.assertTrue(first.request({"cmd": "close", "channel": "ctrl"})["ok"])
                self.assertNotIn("ctrl", first.request({"cmd": "status"})["channels"])
                self.assertIn("ctrl", second.request({"cmd": "status"})["channels"])
                self.assertTrue(second.request({"cmd": "close_connection"})["ok"])
                self.assertNotIn("ctrl", second.request({"cmd": "status"})["channels"])
                self.assertTrue(second.request({"cmd": "shutdown"})["ok"])
            finally:
                if first:
                    first.close()
                if second:
                    second.close()
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=3)
                process.communicate()
                stun.close()

    def test_selected_peer_is_immutable_after_holepunch(self):
        stun = FakeStun()
        peer = MigratingRudpPlayStation()
        stun.start()
        peer.start()
        with tempfile.TemporaryDirectory() as temp:
            ready = Path(temp) / "ready.json"
            process = subprocess.Popen([
                sys.executable, str(RELAY),
                "--listen-address", "127.0.0.1",
                "--control-port", "0",
                "--secret", "relay-test-secret",
                "--stun-server", f"127.0.0.1:{stun.port}",
                "--ready-file", str(ready),
                "--idle-timeout", "30",
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            guest = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            guest.settimeout(2)
            control = None
            try:
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                info = json.loads(ready.read_text())
                control = RelayControl(
                    ("127.0.0.1", info["control_port"]), "relay-test-secret")
                control.authenticate()
                opened = control.request({"cmd": "open", "channel": "ctrl"})
                self.assertTrue(control.request({
                    "cmd": "peers", "channel": "ctrl",
                    "peers": [{"host": "127.0.0.1", "port": peer.port}],
                })["ok"])

                holepunch = bytearray(88)
                holepunch[:4] = struct.pack("!I", 0x06000000)
                holepunch[0x4b:0x50] = b"abcde"
                tunnel = ("127.0.0.1", opened["tunnel_port"])
                guest.sendto(holepunch, tunnel)
                self.assertEqual(len(guest.recv(88)), 88)

                # Retransmitted candidate probes can arrive after holepunch
                # completion. They must not remain queued ahead of RUDP.
                peer.send_duplicate_holepunch()
                peer.send_duplicate_holepunch()
                time.sleep(0.05)

                guest.sendto(rudp_packet(b"request", 0x8030), tunnel)
                response = rudp_packet(b"cookie-response", 0xD000)
                peer.send_migrated(response)
                guest.settimeout(0.2)
                with self.assertRaises(socket.timeout):
                    guest.recv(65535)

                status = control.request({"cmd": "status"})
                self.assertEqual(status["channels"]["ctrl"]["selected_peer"],
                                 f"127.0.0.1:{peer.port}")

                peer.send_noise(b"not-rudp")
                guest.settimeout(0.2)
                with self.assertRaises(socket.timeout):
                    guest.recv(65535)
                status = control.request({"cmd": "status"})
                self.assertEqual(status["channels"]["ctrl"]["selected_peer"],
                                 f"127.0.0.1:{peer.port}")
                self.assertTrue(control.request({"cmd": "shutdown"})["ok"])
            finally:
                guest.close()
                if control:
                    control.close()
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=3)
                process.communicate()
                stun.close()
                peer.close()

    def test_idle_channel_is_removed_from_status(self):
        stun = FakeStun()
        stun.start()
        with tempfile.TemporaryDirectory() as temp:
            ready = Path(temp) / "ready.json"
            process = subprocess.Popen([
                sys.executable, str(RELAY),
                "--listen-address", "127.0.0.1",
                "--control-port", "0",
                "--secret", "relay-test-secret",
                "--stun-server", f"127.0.0.1:{stun.port}",
                "--ready-file", str(ready),
                "--idle-timeout", "0.1",
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            control = None
            try:
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                info = json.loads(ready.read_text())
                control = RelayControl(
                    ("127.0.0.1", info["control_port"]), "relay-test-secret")
                control.authenticate()
                self.assertTrue(control.request({"cmd": "open", "channel": "ctrl"})["ok"])
                time.sleep(0.4)
                self.assertNotIn("ctrl", control.request({"cmd": "status"})["channels"])
                self.assertTrue(control.request({"cmd": "shutdown"})["ok"])
            finally:
                if control:
                    control.close()
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=3)
                process.communicate()
                stun.close()

    def test_rejects_bad_hmac(self):
        self.assertTrue(RELAY.exists(), "relay helper must exist")
        stun = FakeStun()
        stun.start()
        with tempfile.TemporaryDirectory() as temp:
            ready = Path(temp) / "ready.json"
            process = subprocess.Popen([
                sys.executable, str(RELAY),
                "--listen-address", "127.0.0.1",
                "--control-port", "0",
                "--secret", "relay-test-secret",
                "--stun-server", f"127.0.0.1:{stun.port}",
                "--ready-file", str(ready),
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.02)
                info = json.loads(ready.read_text())
                control = RelayControl(
                    ("127.0.0.1", info["control_port"]), "relay-test-secret")
                control.authenticate()
                _, packet = control.encode(
                    {"cmd": "status"}, hmac_override="00" * 32)
                control.sock.send(packet)
                control.sock.settimeout(0.2)
                with self.assertRaises(socket.timeout):
                    control.sock.recv(65535)
                control.sock.settimeout(2)
                self.assertTrue(control.request({"cmd": "shutdown"})["ok"])
                control.close()
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=3)
                process.communicate()
                stun.close()


if __name__ == "__main__":
    unittest.main()
