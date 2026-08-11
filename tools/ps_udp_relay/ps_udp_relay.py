#!/usr/bin/env python3
import argparse
import base64
import hashlib
import hmac
import json
import os
import secrets
import selectors
import socket
import struct
import threading
import time
import urllib.request
import uuid
from datetime import datetime
from pathlib import Path


MAX_CONTROL_DATAGRAM = 60 * 1024
MAGIC_COOKIE = 0x2112A442
PROTOCOL_VERSION = 3
HMAC_LABEL = b"LUNARNX-PS-UDP-RELAY-V3"
SESSION_TTL = 300.0
RESPONSE_TTL = 300.0
SOCKET_BUFFER_SIZE = 4 * 1024 * 1024
EARLY_HOLEPUNCH_TTL = 5.0
MAX_EARLY_HOLEPUNCH_PACKETS = 32
CHIAKI_HOLEPUNCH_PACKET_SIZE = 88
CHIAKI_HOLEPUNCH_REQUEST = 0x06000000
CHIAKI_HOLEPUNCH_RESPONSE = 0x07000000
HOLEPUNCH_RETRY_QUIET_TTL = 0.5
MAX_PSN_SESSION_MESSAGE = 32 * 1024
PSN_SESSION_MESSAGE_URL = (
    "https://web.np.playstation.com/api/sessionManager/v1/"
    "remotePlaySessions/{session_id}/sessionMessage"
)

_LOG_FILE = None


def log(message):
    timestamp = datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    monotonic_ms = time.monotonic_ns() // 1_000_000
    line = f"{timestamp} mono_ms={monotonic_ms} {message}"
    print(line, flush=True)
    if _LOG_FILE is not None:
        _LOG_FILE.write(line + "\n")
        _LOG_FILE.flush()


def compact_json(value):
    return json.dumps(value, separators=(",", ":"))


def describe_session_message(body):
    try:
        envelope = json.loads(body)
        payload = envelope.get("payload", "")
        marker = "body="
        marker_offset = payload.find(marker)
        if marker_offset < 0:
            return "action=unknown req_id=unknown"
        message = json.loads(payload[marker_offset + len(marker):])
        return (f"action={message.get('action', 'unknown')} "
                f"req_id={message.get('reqId', 'unknown')}")
    except (UnicodeDecodeError, json.JSONDecodeError, AttributeError):
        return "action=unparseable req_id=unparseable"


def control_hmac(secret, direction, request_id, nonce, payload):
    message = b"\0".join((
        HMAC_LABEL,
        direction.encode(),
        request_id.encode(),
        nonce.encode(),
        payload.encode(),
    ))
    return hmac.new(secret.encode(), message, hashlib.sha256).hexdigest()


def valid_hex(value, length):
    if not isinstance(value, str) or len(value) != length:
        return False
    try:
        bytes.fromhex(value)
        return True
    except ValueError:
        return False


def parse_endpoint(value):
    host, separator, port_text = value.rpartition(":")
    if not separator or not host:
        raise argparse.ArgumentTypeError("endpoint must be HOST:PORT")
    try:
        socket.inet_aton(host)
        port = int(port_text)
    except (OSError, ValueError) as error:
        raise argparse.ArgumentTypeError("endpoint must use IPv4 and a numeric port") from error
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("port must be in 1..65535")
    return host, port


def post_psn_session_message(token_file, session_id, body, timeout=15.0,
                             url_template=PSN_SESSION_MESSAGE_URL):
    if str(uuid.UUID(session_id)) != session_id:
        raise ValueError("invalid PSN session ID")
    if not isinstance(body, bytes) or not 0 < len(body) <= MAX_PSN_SESSION_MESSAGE:
        raise ValueError("invalid PSN session message body")
    token_data = json.loads(Path(token_file).read_text())
    access_token = token_data.get("access_token", "")
    if not isinstance(access_token, str) or not access_token:
        raise ValueError("PSN token file has no access_token")
    request = urllib.request.Request(
        url_template.format(session_id=session_id),
        data=body,
        headers={
            "Authorization": f"Bearer {access_token}",
            "Content-Type": "application/json; charset=utf-8",
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.status


def is_holepunch_packet(packet):
    if len(packet) != CHIAKI_HOLEPUNCH_PACKET_SIZE:
        return False
    message_type = struct.unpack("!I", packet[:4])[0]
    return message_type in (CHIAKI_HOLEPUNCH_REQUEST,
                            CHIAKI_HOLEPUNCH_RESPONSE)


def stun_mapping(sock, server, timeout=2.0):
    deadline = time.monotonic() + timeout
    server = (socket.gethostbyname(server[0]), server[1])
    transaction = secrets.token_bytes(12)
    request = struct.pack("!HHI", 0x0001, 0, MAGIC_COOKIE) + transaction
    previous_timeout = sock.gettimeout()
    try:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise socket.timeout("STUN resolution exceeded deadline")
        sock.settimeout(remaining)
        sock.sendto(request, server)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise socket.timeout("STUN mapping timed out")
            sock.settimeout(remaining)
            response, source = sock.recvfrom(2048)
            if source != server or len(response) < 20 or response[8:20] != transaction:
                continue
            message_type, length, cookie = struct.unpack("!HHI", response[:8])
            if message_type != 0x0101 or cookie != MAGIC_COOKIE:
                continue
            offset = 20
            end = min(len(response), 20 + length)
            while offset + 4 <= end:
                attr_type, attr_len = struct.unpack("!HH", response[offset:offset + 4])
                value = response[offset + 4:offset + 4 + attr_len]
                if attr_type == 0x0020 and len(value) >= 8 and value[1] == 1:
                    port = struct.unpack("!H", value[2:4])[0] ^ (MAGIC_COOKIE >> 16)
                    encoded_ip = value[4:8]
                    cookie_bytes = struct.pack("!I", MAGIC_COOKIE)
                    host = socket.inet_ntoa(bytes(a ^ b for a, b in zip(encoded_ip, cookie_bytes)))
                    return host, port
                offset += 4 + ((attr_len + 3) & ~3)
            raise RuntimeError("STUN response has no IPv4 XOR-MAPPED-ADDRESS")
    finally:
        sock.settimeout(previous_timeout)


class RelayChannel:
    def __init__(self, connection_id, name, listen_address, stun_server,
                 idle_timeout, verbose):
        self.connection_id = connection_id
        self.name = name
        self.external = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.external.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF,
                                 SOCKET_BUFFER_SIZE)
        self.external.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF,
                                 SOCKET_BUFFER_SIZE)
        # The guest-facing bridge is not an internet-routable source address.
        # Let macOS choose the outbound interface for STUN and PS5 traffic.
        self.external.bind(("0.0.0.0", 0))
        self.mapped = stun_mapping(self.external, stun_server)
        self.external.setblocking(False)
        self.tunnel = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.tunnel.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF,
                               SOCKET_BUFFER_SIZE)
        self.tunnel.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF,
                               SOCKET_BUFFER_SIZE)
        self.tunnel.bind((listen_address, 0))
        self.tunnel.setblocking(False)
        self.peers = []
        self.selected_peer = None
        self.guest = None
        self.request_ids = set()
        self.last_activity = time.monotonic()
        self.idle_timeout = idle_timeout
        self.verbose = verbose
        self.packets_in = 0
        self.packets_out = 0
        self.holepunch_duplicates_dropped = 0
        self.holepunch_retries_coalesced = 0
        self.holepunch_responses_accelerated = 0
        self.forwarded_holepunch_packets = set()
        self.guest_request_template = None
        self.ps_request_in_flight = None
        self.ps_request_completed_at = 0.0
        self.pending_external = []
        self.running = True
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._run, name=f"ps-relay-{name}", daemon=True)
        self.thread.start()

    @property
    def tunnel_port(self):
        return self.tunnel.getsockname()[1]

    def set_peers(self, peers):
        parsed = []
        for peer in peers:
            host = str(peer.get("host", ""))
            port = int(peer.get("port", 0))
            socket.inet_aton(host)
            if not 1 <= port <= 65535:
                raise ValueError("invalid peer port")
            parsed.append((host, port))
        if not parsed:
            raise ValueError("at least one peer is required")
        with self.lock:
            self.peers = parsed
            if self.selected_peer and self.selected_peer not in parsed:
                self.selected_peer = None

    @property
    def alive(self):
        return self.running and self.thread.is_alive()

    def _remember_guest_request(self, packet):
        if len(packet) != CHIAKI_HOLEPUNCH_PACKET_SIZE:
            return
        if struct.unpack("!I", packet[:4])[0] != CHIAKI_HOLEPUNCH_REQUEST:
            return
        with self.lock:
            self.request_ids.add(packet[0x4b:0x50])
            self.guest_request_template = bytes(packet)

    def _can_select_source(self, packet, source):
        if len(packet) != CHIAKI_HOLEPUNCH_PACKET_SIZE:
            return False
        message_type = struct.unpack("!I", packet[:4])[0]
        with self.lock:
            peer_hosts = {peer[0] for peer in self.peers}
            request_ids = set(self.request_ids)
        if source[0] not in peer_hosts:
            return False
        if message_type == CHIAKI_HOLEPUNCH_REQUEST:
            return source in self.peers
        return (message_type == CHIAKI_HOLEPUNCH_RESPONSE
                and packet[0x4b:0x50] in request_ids)

    def status(self):
        with self.lock:
            return {
                "mapped": f"{self.mapped[0]}:{self.mapped[1]}",
                "tunnel_port": self.tunnel_port,
                "selected_peer": (f"{self.selected_peer[0]}:{self.selected_peer[1]}"
                                  if self.selected_peer else None),
                "packets_in": self.packets_in,
                "packets_out": self.packets_out,
                "holepunch_duplicates_dropped": self.holepunch_duplicates_dropped,
                "holepunch_retries_coalesced": self.holepunch_retries_coalesced,
                "holepunch_responses_accelerated": self.holepunch_responses_accelerated,
            }

    def _observe_guest_holepunch(self, packet):
        if len(packet) != CHIAKI_HOLEPUNCH_PACKET_SIZE:
            return
        if struct.unpack("!I", packet[:4])[0] != CHIAKI_HOLEPUNCH_RESPONSE:
            return
        request_id = packet[0x4b:0x50]
        with self.lock:
            if request_id == self.ps_request_in_flight:
                self.ps_request_in_flight = None
                self.ps_request_completed_at = time.monotonic()

    def _should_forward_external(self, packet):
        if not is_holepunch_packet(packet):
            return True
        message_type = struct.unpack("!I", packet[:4])[0]
        request_id = packet[0x4b:0x50]
        now = time.monotonic()
        with self.lock:
            if packet in self.forwarded_holepunch_packets:
                self.holepunch_duplicates_dropped += 1
                dropped = self.holepunch_duplicates_dropped
                if self.verbose and (dropped <= 3 or dropped % 10 == 0):
                    log(f"[{self.name}] dropped stale holepunch packet "
                        f"count={dropped}")
                return False
            if message_type == CHIAKI_HOLEPUNCH_REQUEST:
                request_pending = self.ps_request_in_flight is not None
                response_just_forwarded = (
                    self.ps_request_completed_at > 0
                    and now - self.ps_request_completed_at
                    < HOLEPUNCH_RETRY_QUIET_TTL
                )
                if request_pending or response_just_forwarded:
                    self.holepunch_retries_coalesced += 1
                    return False
                self.ps_request_in_flight = request_id
            self.forwarded_holepunch_packets.add(packet)
        return True

    def _build_accelerated_response(self, packet, source):
        if len(packet) != CHIAKI_HOLEPUNCH_PACKET_SIZE:
            return None
        if struct.unpack("!I", packet[:4])[0] != CHIAKI_HOLEPUNCH_REQUEST:
            return None
        with self.lock:
            template = self.guest_request_template
        if template is None:
            return None

        response = bytearray(CHIAKI_HOLEPUNCH_PACKET_SIZE)
        response[:4] = struct.pack("!I", CHIAKI_HOLEPUNCH_RESPONSE)
        response[0x04:0x48] = template[0x04:0x48]
        response[0x4b:0x50] = packet[0x4b:0x50]
        response[0x50:0x52] = template[0x44:0x46]
        response[0x52:0x54] = template[0x46:0x48]
        response[0x54:0x56] = template[0x44:0x46]
        peer_address = socket.inet_aton(source[0])
        peer_port = struct.pack("!H", source[1])
        for offset, value in enumerate(peer_address):
            response[0x50 + offset] ^= value
        for offset, value in enumerate(peer_port):
            response[0x54 + offset] ^= value
        return bytes(response)

    def _forward_external(self, packet, source, guest):
        if not self._should_forward_external(packet):
            return False
        accelerated = self._build_accelerated_response(packet, source)
        if accelerated is not None:
            self.external.sendto(accelerated, source)
            with self.lock:
                self.packets_out += 1
                self.holepunch_responses_accelerated += 1
        self.tunnel.sendto(packet, guest)
        with self.lock:
            self.packets_in += 1
        return True

    def _select_source(self, packet, source):
        with self.lock:
            selected_peer = self.selected_peer
        if selected_peer is None and self._can_select_source(packet, source):
            with self.lock:
                if self.selected_peer is None:
                    self.selected_peer = source
                    log(f"[{self.connection_id}/{self.name}] selected peer "
                        f"{source[0]}:{source[1]}")
                selected_peer = self.selected_peer
        return selected_peer

    def _buffer_early_external(self, packet, source):
        if not is_holepunch_packet(packet):
            return
        now = time.monotonic()
        with self.lock:
            self.pending_external = [
                pending for pending in self.pending_external
                if now - pending[0] <= EARLY_HOLEPUNCH_TTL
            ]
            self.pending_external.append((now, packet, source))
            self.pending_external = self.pending_external[
                -MAX_EARLY_HOLEPUNCH_PACKETS:]

    def _flush_early_external(self):
        now = time.monotonic()
        with self.lock:
            pending = self.pending_external
            self.pending_external = []
        retained = []
        for received_at, packet, source in pending:
            if now - received_at > EARLY_HOLEPUNCH_TTL:
                continue
            selected_peer = self._select_source(packet, source)
            with self.lock:
                guest = self.guest
            if source == selected_peer and guest:
                self._forward_external(packet, source, guest)
            else:
                retained.append((received_at, packet, source))
        if retained:
            with self.lock:
                self.pending_external = (
                    retained + self.pending_external
                )[-MAX_EARLY_HOLEPUNCH_PACKETS:]

    def _run(self):
        selector = selectors.DefaultSelector()
        selector.register(self.tunnel, selectors.EVENT_READ, "guest")
        selector.register(self.external, selectors.EVENT_READ, "external")
        try:
            while self.running:
                if time.monotonic() - self.last_activity >= self.idle_timeout:
                    break
                for key, _ in selector.select(0.25):
                    try:
                        packet, source = key.fileobj.recvfrom(65535)
                    except OSError:
                        continue
                    self.last_activity = time.monotonic()
                    if key.data == "guest":
                        if self.verbose and is_holepunch_packet(packet):
                            log(f"[DEBUG-ps-relay-order] {self.name} guest->ps "
                                f"type={struct.unpack('!I', packet[:4])[0]:08x} "
                                f"request={packet[0x4b:0x50].hex()}")
                        self._remember_guest_request(packet)
                        self._observe_guest_holepunch(packet)
                        with self.lock:
                            self.guest = source
                            targets = ([self.selected_peer] if self.selected_peer
                                       else list(self.peers))
                        self._flush_early_external()
                        for target in targets:
                            if target:
                                self.external.sendto(packet, target)
                                with self.lock:
                                    self.packets_out += 1
                    else:
                        if self.verbose and is_holepunch_packet(packet):
                            log(f"[DEBUG-ps-relay-order] {self.name} ps->guest "
                                f"type={struct.unpack('!I', packet[:4])[0]:08x} "
                                f"request={packet[0x4b:0x50].hex()}")
                        with self.lock:
                            guest = self.guest
                        selected_peer = self._select_source(packet, source)
                        if selected_peer is None or not guest:
                            self._buffer_early_external(packet, source)
                        if source == selected_peer and guest:
                            self._forward_external(packet, source, guest)
        finally:
            self.running = False
            selector.close()
            self.external.close()
            self.tunnel.close()

    def close(self):
        self.running = False
        self.thread.join(timeout=1)
        if self.verbose:
            with self.lock:
                log(f"[{self.name}] closed packets_from_ps={self.packets_in} "
                    f"packets_to_ps={self.packets_out}")


class RelayServer:
    def __init__(self, args):
        self.args = args
        self.stun_server = (
            socket.gethostbyname(args.stun_server[0]), args.stun_server[1])
        self.channels = {}
        self.sessions = {}
        self.responses = {}
        self.shutdown_requested = False
        if args.verbose:
            log(f"STUN server resolved to {self.stun_server[0]}:{self.stun_server[1]}")

    def cleanup_channels(self):
        expired = [key for key, channel in self.channels.items()
                   if not channel.alive]
        for key in expired:
            channel = self.channels.pop(key)
            channel.close()
            log(f"[{key[0]}/{key[1]}] expired after idle timeout")

    @staticmethod
    def channel_key(request):
        connection_id = request.get("connection_id")
        if not valid_hex(connection_id, 32):
            raise ValueError("connection_id must be 16 random bytes in hex")
        name = request.get("channel")
        if name not in ("ctrl", "data"):
            raise ValueError("channel must be ctrl or data")
        return connection_id, name

    def response(self, request):
        self.cleanup_channels()
        command = request.get("cmd")
        if command == "open":
            connection_id, name = self.channel_key(request)
            key = (connection_id, name)
            if key in self.channels:
                self.channels[key].close()
            started = time.monotonic()
            channel = RelayChannel(connection_id, name, self.args.listen_address,
                                   self.stun_server, self.args.idle_timeout,
                                   self.args.verbose)
            self.channels[key] = channel
            log(f"[{connection_id}/{name}] mapped "
                f"{channel.mapped[0]}:{channel.mapped[1]} "
                f"in {time.monotonic() - started:.3f}s")
            return {"ok": True, "mapped_host": channel.mapped[0],
                    "mapped_port": channel.mapped[1],
                    "tunnel_port": channel.tunnel_port}
        if command == "peers":
            key = self.channel_key(request)
            channel = self.channels.get(key)
            if not channel:
                raise ValueError("channel is not open")
            channel.set_peers(request.get("peers", []))
            return {"ok": True}
        if command == "status":
            connection_id = request.get("connection_id")
            if not valid_hex(connection_id, 32):
                raise ValueError("connection_id must be 16 random bytes in hex")
            return {"ok": True, "channels": {
                name: channel.status()
                for (owner, name), channel in self.channels.items()
                if owner == connection_id
            }}
        if command == "session_message":
            if not self.args.psn_token_file:
                raise ValueError("PSN session message proxy is not configured")
            session_id = request.get("session_id", "")
            body_base64 = request.get("body_base64")
            if not isinstance(body_base64, str):
                raise ValueError("PSN session message body must be base64")
            try:
                body = base64.b64decode(body_base64, validate=True)
            except (ValueError, base64.binascii.Error) as error:
                raise ValueError("invalid PSN session message base64") from error
            started = time.monotonic()
            message_description = describe_session_message(body)
            log(f"PSN session message forwarding {message_description}")
            status = post_psn_session_message(
                self.args.psn_token_file, session_id, body)
            elapsed_ms = (time.monotonic() - started) * 1000
            log(f"PSN session message forwarded {message_description} "
                f"status={status} "
                f"elapsed_ms={elapsed_ms:.1f}")
            return {"ok": True, "status": status,
                    "elapsed_ms": round(elapsed_ms, 1)}
        if command == "close":
            channel = self.channels.pop(self.channel_key(request), None)
            if channel:
                channel.close()
            return {"ok": True}
        if command == "close_connection":
            connection_id = request.get("connection_id")
            if not valid_hex(connection_id, 32):
                raise ValueError("connection_id must be 16 random bytes in hex")
            keys = [key for key in self.channels if key[0] == connection_id]
            for key in keys:
                self.channels.pop(key).close()
            return {"ok": True}
        if command == "shutdown":
            self.shutdown_requested = True
            return {"ok": True}
        raise ValueError("unknown command")

    def signed_response(self, request_id, nonce, value):
        payload = compact_json(value)
        envelope = {
            "version": PROTOCOL_VERSION,
            "request_id": request_id,
            "nonce": nonce,
            "payload": payload,
            "hmac": control_hmac(
                self.args.secret, "response", request_id, nonce, payload),
        }
        return compact_json(envelope).encode()

    def cleanup_control_state(self, now):
        self.sessions = {
            key: expiry for key, expiry in self.sessions.items()
            if expiry > now
        }
        self.responses = {
            key: value for key, value in self.responses.items()
            if value[2] > now
        }

    def handle_datagram(self, packet, address):
        now = time.monotonic()
        self.cleanup_control_state(now)
        if not packet or len(packet) > MAX_CONTROL_DATAGRAM:
            return None
        try:
            envelope = json.loads(packet)
        except (UnicodeDecodeError, json.JSONDecodeError):
            return None
        if not isinstance(envelope, dict) or envelope.get("version") != PROTOCOL_VERSION:
            return None
        request_id = envelope.get("request_id")
        payload = envelope.get("payload")
        if not valid_hex(request_id, 32) or not isinstance(payload, str):
            return None

        cache_key = (address, request_id)
        fingerprint = hashlib.sha256(packet).digest()
        cached = self.responses.get(cache_key)
        if cached:
            if hmac.compare_digest(cached[0], fingerprint):
                if self.args.verbose:
                    log(f"Control retry request_id={request_id} returning cached response")
                return cached[1]
            log(f"Control request ID collision from {address[0]}:{address[1]}")
            return None

        try:
            request = json.loads(payload)
        except json.JSONDecodeError:
            return None
        if not isinstance(request, dict):
            return None
        command = request.get("cmd")

        if command == "challenge":
            nonce = secrets.token_hex(32)
            self.sessions[(address, nonce)] = now + SESSION_TTL
            response_packet = self.signed_response(
                request_id, nonce, {"ok": True})
            self.responses[cache_key] = (
                fingerprint, response_packet, now + RESPONSE_TTL)
            if self.args.verbose:
                log(f"Control challenge issued to {address[0]}:{address[1]}")
            return response_packet

        nonce = envelope.get("nonce")
        digest = envelope.get("hmac")
        session_key = (address, nonce)
        if not valid_hex(nonce, 64) or self.sessions.get(session_key, 0) <= now:
            return None
        expected = control_hmac(
            self.args.secret, "request", request_id, nonce, payload)
        if not hmac.compare_digest(str(digest), expected):
            log(f"Control authentication failed from {address[0]}:{address[1]}")
            return None
        self.sessions[session_key] = now + SESSION_TTL

        started = time.monotonic()
        if self.args.verbose:
            log(f"Control command {command} channel={request.get('channel', '-')} received")
        try:
            response = self.response(request)
        except Exception as error:
            log(f"Control command failed: {type(error).__name__}: {error}")
            response = {"ok": False, "error": str(error)}
        response_packet = self.signed_response(request_id, nonce, response)
        self.responses[cache_key] = (
            fingerprint, response_packet, now + RESPONSE_TTL)
        if self.args.verbose:
            log(f"Control response sent in {time.monotonic() - started:.3f}s")
        return response_packet

    def close(self):
        for channel in list(self.channels.values()):
            channel.close()
        self.channels.clear()


def parse_args():
    parser = argparse.ArgumentParser(description="Ryubing PlayStation UDP relay")
    parser.add_argument("--listen-address", required=True)
    parser.add_argument("--control-port", type=int, default=47998)
    parser.add_argument("--secret", required=True)
    parser.add_argument("--stun-server", type=parse_endpoint,
                        default=("stun.moonlight-stream.org", 3478))
    parser.add_argument("--idle-timeout", type=float, default=300.0)
    parser.add_argument("--ready-file", type=Path)
    parser.add_argument("--psn-token-file", type=Path)
    parser.add_argument("--log-file", type=Path,
                        help="also write timestamped relay diagnostics here")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    socket.inet_aton(args.listen_address)
    if not 0 <= args.control_port <= 65535:
        parser.error("control port must be in 0..65535")
    if len(args.secret) < 12:
        parser.error("secret must contain at least 12 characters")
    if args.idle_timeout <= 0:
        parser.error("idle timeout must be positive")
    return args


def main():
    global _LOG_FILE
    args = parse_args()
    if args.log_file:
        args.log_file.parent.mkdir(parents=True, exist_ok=True)
        _LOG_FILE = args.log_file.open("a", encoding="utf-8")
    server = RelayServer(args)
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.listen_address, args.control_port))
    listener.settimeout(0.25)
    port = listener.getsockname()[1]
    if args.ready_file:
        args.ready_file.write_text(json.dumps({"control_port": port}) + "\n")
    log(f"PS UDP relay control listening on UDP {args.listen_address}:{port}")
    try:
        while not server.shutdown_requested:
            try:
                packet, address = listener.recvfrom(MAX_CONTROL_DATAGRAM)
            except socket.timeout:
                continue
            response = server.handle_datagram(packet, address)
            if response:
                sent = listener.sendto(response, address)
                if args.verbose:
                    log(f"control response_bytes={len(response)} "
                        f"sent_bytes={sent} to={address[0]}:{address[1]}")
    finally:
        server.close()
        listener.close()
        if _LOG_FILE is not None:
            _LOG_FILE.close()
            _LOG_FILE = None
        if args.ready_file:
            try:
                os.unlink(args.ready_file)
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    main()
