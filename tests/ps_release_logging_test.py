#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    makefile = (ROOT / "Makefile.switch").read_text()
    adapter = (ROOT / "src/ps/chiaki_log_adapter.cpp").read_text()
    chiaki_docker_build = (
        ROOT / "tools/chiaki_switch/build_in_docker.sh").read_text()
    chiaki_container_build = (
        ROOT / "tools/chiaki_switch/build_in_container.sh").read_text()
    registration = (ROOT / "src/ps/ps_registration.cpp").read_text()
    manager = (ROOT / "src/ps/ps_manager.cpp").read_text()

    assert "APP_DIAG ?= 0" in makefile
    assert "DROP_DIAG ?= 1" in makefile
    assert "#if LUNARNX_DIAGNOSTIC_LOG\n    ensureDiagnosticLogDirectory();" in adapter
    assert "#if LUNARNX_DIAGNOSTIC_LOG || LUNARNX_DROP_DIAGNOSTIC_LOG" in adapter
    assert "ChiakiLog log{};" in adapter
    assert ('-e CHIAKI_TRANSPORT_DIAG="${CHIAKI_TRANSPORT_DIAG:-0}"' in
            chiaki_docker_build)
    assert ('chiaki_transport_diag="${CHIAKI_TRANSPORT_DIAG:-0}"' in
            chiaki_container_build)
    assert 'persistentEventLog("ps-registration"' in registration
    assert 'stage=missing-account-id' in manager
    assert 'stage=credential-save' in manager
    assert "chiaki_log_sniffer_init(&log_sniffer_,\n        CHIAKI_LOG_ALL" in registration
    assert "formatRegistrationEvent" in registration

    print("PS release logging test passed")


if __name__ == "__main__":
    main()
