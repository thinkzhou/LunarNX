#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    makefile = (ROOT / "Makefile.switch").read_text()
    adapter = (ROOT / "src/ps/chiaki_log_adapter.cpp").read_text()
    chiaki_build = (ROOT / "tools/chiaki_switch/build_in_docker.sh").read_text()

    assert "APP_DIAG ?= 0" in makefile
    assert "DROP_DIAG ?= 0" in makefile
    assert "#if LUNARNX_DIAGNOSTIC_LOG\n    ensureDiagnosticLogDirectory();" in adapter
    assert "#if LUNARNX_DIAGNOSTIC_LOG || LUNARNX_DROP_DIAGNOSTIC_LOG" in adapter
    assert "ChiakiLog log{};" in adapter
    assert "CHIAKI_TRANSPORT_DIAG=${CHIAKI_TRANSPORT_DIAG:-0}" in chiaki_build

    print("PS release logging test passed")


if __name__ == "__main__":
    main()
