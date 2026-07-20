#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    source = Path("lib/libpeer/src/dtls_srtp.c").read_text()
    header = Path("lib/libpeer/src/dtls_srtp.h").read_text()
    peer_connection = Path("lib/libpeer/src/peer_connection.c").read_text()
    asn1write = Path("lib/libpeer/third_party/mbedtls/library/asn1write.c").read_text()
    switch_makefile = Path("Makefile.switch").read_text()
    docker_build = Path("scripts/docker_build_full.sh").read_text()
    switch_stubs = Path("src/platform/switch_posix_stubs.c").read_text()

    disabled = "mbedtls_ssl_conf_dtls_cookies(&dtls_srtp->conf, NULL, NULL, NULL)" in source
    require(disabled, "WebRTC DTLS server cookie callbacks should be disabled")

    active_cookie_callbacks = (
        "mbedtls_ssl_conf_dtls_cookies(&dtls_srtp->conf, mbedtls_ssl_cookie_write" in source
    )
    require(not active_cookie_callbacks,
            "WebRTC DTLS should not send HelloVerifyRequest cookie retries")

    require("*flags = 0;" in source,
            "WebRTC DTLS certificates should be authenticated by the SDP fingerprint, not PKIX flags")
    require("actual_remote_fingerprint" in source and
            "remote_fingerprint" in source and
            "Fingerprint mismatch" in source,
            "PKIX bypass must retain the post-handshake SDP fingerprint check")

    require("mbedtls_x509write_crt_set_serial_raw" not in source,
            "DTLS self-signed certificate should not use raw string serials")
    require("mbedtls_mpi_read_binary(&serial" in source and
            "mbedtls_x509write_crt_set_serial(&crt, &serial)" in source,
            "DTLS self-signed certificate should use an MPI serial like libdatachannel")

    require("MBEDTLS_OID_ECDSA_SHA256" in asn1write and
            "mbedtls_asn1_algorithm_identifier_omits_params" in asn1write,
            "ECDSA AlgorithmIdentifier parameters should be omitted for strict WebRTC peers")

    require("DtlsSrtpTimer timer;" in header,
            "DTLS timer callback storage must live as long as the DtlsSrtp session")
    require("DtlsSrtpTimer timer = {0};" not in source,
            "DTLS timer callback must not point at a handshake stack variable")
    require("mbedtls_ssl_set_timer_cb(&dtls_srtp->ssl, &dtls_srtp->timer" in source,
            "DTLS timer callback should use the session-owned timer")

    require("dtls_srtp_set_role" in header and "dtls_srtp_set_role" in source,
            "DTLS should support applying the role selected by the remote answer")
    require('return "a=setup:actpass"' in peer_connection,
            "WebRTC offers should advertise actpass like XStreaming/libwebrtc")
    require("dtls_srtp_set_role(&pc->dtls_srtp, role)" in peer_connection,
            "Remote setup attributes should reconfigure the local DTLS role")

    for source_name, build_source in (("Makefile.switch", switch_makefile),
                                      ("Docker mbedTLS build", docker_build)):
        require("MBEDTLS_NO_PLATFORM_ENTROPY" in build_source and
                "MBEDTLS_ENTROPY_HARDWARE_ALT" in build_source,
                f"{source_name} must use the Switch hardware entropy configuration")
    require("mbedtls_hardware_poll" in switch_stubs and
            "randomGet(output, len)" in switch_stubs,
            "Switch mbedTLS entropy must come from libnx randomGet")

    print("libpeer DTLS config tests passed")


if __name__ == "__main__":
    main()
