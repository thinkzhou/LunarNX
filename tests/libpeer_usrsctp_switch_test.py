#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    switch_makefile = Path("Makefile.switch").read_text()
    build_script = Path("scripts/build_switch_in_container.sh").read_text()
    sctp = Path("lib/libpeer/src/sctp.c").read_text()
    sctp_header = Path("lib/libpeer/src/sctp.h").read_text()
    peer_connection = Path("lib/libpeer/src/peer_connection.c").read_text()
    legacy_patch = Path(
        "tools/libpeer_legacy/legacy-libpeer-switch.patch"
    ).read_text()
    protocol_patch = Path(
        "tools/libpeer_legacy/0003-enable-usrsctp-datachannel-policy.patch"
    ).read_text()
    usrsctp_patch = Path(
        "tools/libpeer_legacy/nested/0001-switch-add-libnx-compatibility-stubs.patch"
    ).read_text()

    legacy_branch = switch_makefile[
        switch_makefile.index("else\nLIBPEER       = lib/libpeer"):
        switch_makefile.index("endif\n\nNETWORK_DIAG")
    ]
    require("USRSCTP_CFLAGS = -DCONFIG_USE_USRSCTP=1" in legacy_branch,
            "legacy Switch WebRTC must use the full usrsctp stack")
    require("USRSCTP_LIBS   = -lusrsctp" in legacy_branch,
            "legacy Switch WebRTC must link libusrsctp")
    require("Building usrsctp" in build_script and
            "-Dsctp_inet6=0" in build_script,
            "the canonical Switch build must build the patched IPv4 usrsctp archive")
    require("-DENABLE_MBEDTLS=ON" in build_script and
            "-DMBEDTLS_INCLUDE_DIRS=\"$mbedtls_dir/include\"" in build_script and
            "-DMBEDTLS_LIBRARY=\"$mbedtls_dir/build_switch/library/libmbedtls.a\"" in build_script and
            "-DMBEDX509_LIBRARY=\"$mbedtls_dir/build_switch/library/libmbedx509.a\"" in build_script and
            "-DMBEDCRYPTO_LIBRARY=\"$mbedtls_dir/build_switch/library/libmbedcrypto.a\"" in build_script,
            "the canonical Switch libsrtp build must use the existing mbedTLS backend")
    require("ENABLE_MBEDTLS:BOOL=ON" in build_script,
            "the canonical Switch build must fail if libsrtp silently disables mbedTLS")
    require("aes_icm_mbedtls.o" in build_script and
            "aes_gcm_mbedtls.o" in build_script and
            "hmac_mbedtls.o" in build_script,
            "the canonical Switch build must verify the mbedTLS crypto objects")

    require("defined(__SWITCH__)" in usrsctp_patch and
            "struct sockaddr_conn" in usrsctp_patch,
            "Switch usrsctp must use the BSD sockaddr_conn layout")
    require("randomGet" in usrsctp_patch and
            "read_random" in usrsctp_patch,
            "Switch usrsctp must use libnx's kernel CSPRNG")

    require("channel_type" in sctp_header and
            "reliability_parameter" in sctp_header,
            "SCTP stream mappings must retain negotiated DataChannel policy")
    require("#define SCTP_MAX_STREAMS 16" in sctp_header,
            "legacy libpeer must retain enough local DataChannel stream mappings")
    require("-#define SCTP_MAX_STREAMS 5" in legacy_patch and
            "+#define SCTP_MAX_STREAMS 16" in legacy_patch,
            "the reproducible legacy libpeer patch must preserve the 16-stream table")
    require("SCTP_SEND_PRINFO_VALID" in sctp and
            "SCTP_PR_SCTP_RTX" in sctp and
            "SCTP_UNORDERED" in sctp,
            "usrsctp sends must apply zero-retransmit and unordered policy")
    require("sctp_add_stream_mapping(&pc->sctp, label, sid," in peer_connection,
            "locally opened channels must register their send policy")
    require("SCTP_SEND_PRINFO_VALID" in protocol_patch and
            "SCTP_PR_SCTP_RTX" in protocol_patch and
            "SCTP_UNORDERED" in protocol_patch,
            "the reproducible legacy libpeer patch must preserve PR-SCTP policy")

    callback_start = sctp.index("static int sctp_outgoing_data_cb")
    callback_end = sctp.index("int sctp_outgoing_data(", callback_start)
    callback = sctp[callback_start:callback_end]
    require("const int write_result = dtls_srtp_write" in callback and
            "if (write_result >= 0)" in callback and
            "return 0;" in callback and
            "return EAGAIN;" in callback and
            "return EIO;" in callback,
            "usrsctp output must translate DTLS writes to 0-or-errno callback semantics")
    require("return dtls_srtp_write" not in callback,
            "a successful DTLS byte count must not be reported as an usrsctp error")
    require("const int write_result = dtls_srtp_write" in legacy_patch and
            "return EAGAIN;" in legacy_patch and
            "return EIO;" in legacy_patch,
            "the reproducible legacy patch must preserve the usrsctp output contract")

    require("usrsctp_initialized" in sctp and
            "usrsctp_finish() == 0" in sctp,
            "usrsctp global lifecycle must not double-initialize after a busy shutdown")
    require("sctp->sock = sock;" in sctp and
            "usrsctp_deregister_address(sctp);" in sctp,
            "usrsctp association failures and reconnects must release socket state")
    require("usrsctp_initialized" in protocol_patch and
            "usrsctp_deregister_address(sctp);" in protocol_patch,
            "the reproducible legacy libpeer patch must preserve reconnect cleanup")

    print("legacy Switch usrsctp tests passed")


if __name__ == "__main__":
    main()
