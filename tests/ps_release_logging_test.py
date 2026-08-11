#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    makefile = (ROOT / "Makefile.switch").read_text()
    adapter = (ROOT / "src/ps/chiaki_log_adapter.cpp").read_text()

    assert "APP_DIAG ?= 0" in makefile
    assert "DROP_DIAG ?= 0" in makefile
    assert "#if LUNARNX_DIAGNOSTIC_LOG\n    ensureDiagnosticLogDirectory();" in adapter
    assert "#if LUNARNX_DIAGNOSTIC_LOG || LUNARNX_DROP_DIAGNOSTIC_LOG" in adapter
    assert "ChiakiLog log{};" in adapter

    print("PS release logging test passed")


if __name__ == "__main__":
    main()
