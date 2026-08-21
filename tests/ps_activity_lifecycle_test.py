#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/ui/ps_activity.cpp").read_text()


def main() -> None:
    resume = SOURCE.split("void PsActivity::onResume()", 1)[1].split(
        "void PsActivity::startLanDiscovery()", 1
    )[0]
    sync_start = resume.index("brls::sync([this, alive]()")
    assert "if (source_ == PsConsoleSource::Local) startLanDiscovery();" in \
        resume[sync_start:]
    assert "if (source_ == PsConsoleSource::Local) startLanDiscovery();" not in \
        resume[:sync_start]
    assert "if (!alive->load() || console_list_refresh_suspended_) return;" in resume

    discovery = SOURCE.split("void PsActivity::startLanDiscovery()", 1)[1]
    assert "rebuildConsoleList(ps_manager_->getDiscoveredHosts());" not in discovery
    assert "console_list_refresh_suspended_ = true;" in SOURCE
    assert "stopDiscovery();" in SOURCE.split("void PsActivity::onPause()", 1)[1].split(
        "void PsActivity::onResume()", 1
    )[0]
    print("PS activity lifecycle test passed")


if __name__ == "__main__":
    main()
