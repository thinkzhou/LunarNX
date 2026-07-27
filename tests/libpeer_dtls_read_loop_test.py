#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    peer_connection = (ROOT / "lib/libpeer/src/peer_connection.c").read_text()
    dtls_srtp = (ROOT / "lib/libpeer/src/dtls_srtp.c").read_text()

    assert "#define PEER_CONNECTION_MAX_PACKETS_PER_LOOP" in peer_connection
    assert "pc->agent_ret = 0;" in peer_connection
    assert "pc->state == PEER_CONNECTION_COMPLETED" in peer_connection
    assert "MBEDTLS_ERR_SSL_WANT_READ" in peer_connection
    assert "return MBEDTLS_ERR_SSL_WANT_READ;" in peer_connection
    assert "dtls_srtp_has_pending(&pc->dtls_srtp)" in peer_connection
    assert "PEER_CONNECTION_MAX_DTLS_PENDING_READS" in peer_connection
    pending_start = peer_connection.index(
        "static int peer_connection_drain_pending_dtls")
    pending_end = peer_connection.index(
        "static void peer_connection_outgoing_rtp_packet", pending_start)
    pending_impl = peer_connection[pending_start:pending_end]
    assert "while (processed < budget" in pending_impl
    assert "dtls_srtp_has_pending(&pc->dtls_srtp)" in pending_impl
    completed_start = peer_connection.index("case PEER_CONNECTION_COMPLETED")
    completed = peer_connection[completed_start:]
    first_pending = completed.index("peer_connection_drain_pending_dtls")
    first_receive = completed.index("agent_recv(&pc->agent")
    assert first_pending < first_receive
    assert "return ret > 0 ? ret : MBEDTLS_ERR_SSL_WANT_READ;" in peer_connection
    assert "DTLS handshake failed ret=" in peer_connection
    assert "[DEBUG-mbedtls]" in dtls_srtp
    recv_start = peer_connection.index("static int peer_connection_dtls_srtp_recv")
    recv_end = peer_connection.index("static int peer_connection_dtls_srtp_send")
    recv_impl = peer_connection[recv_start:recv_end]
    assert "CONFIG_TLS_READ_TIMEOUT" not in recv_impl
    assert "while (" not in recv_impl

    read_start = dtls_srtp.index("int dtls_srtp_read")
    read_end = dtls_srtp.index("int dtls_srtp_probe")
    read_impl = dtls_srtp[read_start:read_end]
    assert "while (ret == MBEDTLS_ERR_SSL_WANT_READ" not in read_impl
    assert "mbedtls_ssl_read" in read_impl
    assert "int dtls_srtp_has_pending" in dtls_srtp
    assert "mbedtls_ssl_check_pending" in dtls_srtp
    handshake_start = dtls_srtp.index("static int dtls_srtp_do_handshake")
    handshake_end = dtls_srtp.index("static int dtls_srtp_handshake_server")
    handshake_impl = dtls_srtp[handshake_start:handshake_end]
    assert "return mbedtls_ssl_handshake(&dtls_srtp->ssl);" in handshake_impl
    assert "do {" not in handshake_impl

    print("libpeer DTLS read loop tests passed")


if __name__ == "__main__":
    main()
