#!/usr/bin/env python3
import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "psn_remote_probe", ROOT / "tools/psn_remote_probe.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def complete_phases():
    phases = [
        {"phase": phase, "status": status}
        for phase, status in MODULE.FULL_FLOW_PHASES
    ]
    phases[-1].update({
        "connected": True,
        "registered": True,
        "data_hole": True,
        "clean_stop": True,
        "video_frames": 120,
        "audio_frames": 220,
        "frames_lost": 0,
    })
    return phases


class FullFlowGateTest(unittest.TestCase):
    def test_complete_direct_flow_passes(self):
        result = MODULE.validate_full_flow(complete_phases(), 0)
        self.assertTrue(result["passed"], result["errors"])

    def test_relay_flow_requires_relay_configuration_phase(self):
        result = MODULE.validate_full_flow(
            complete_phases(), 0, relay_expected=True)
        self.assertFalse(result["passed"])
        self.assertIn("relay_config:ok is missing", result["errors"])

    def test_relay_flow_passes_with_configuration_phase(self):
        phases = complete_phases()
        phases.insert(2, {"phase": "relay_config", "status": "ok"})
        result = MODULE.validate_full_flow(phases, 0, relay_expected=True)
        self.assertTrue(result["passed"], result["errors"])

    def test_missing_media_or_runtime_failure_fails(self):
        phases = complete_phases()
        phases[-1]["video_frames"] = 0
        phases[-1]["frames_lost"] = 1
        result = MODULE.validate_full_flow(phases, 7)
        self.assertFalse(result["passed"])
        self.assertIn("media_summary.video_frames is not positive", result["errors"])
        self.assertIn("media_summary.frames_lost is not zero", result["errors"])
        self.assertIn("native probe exit code is 7", result["errors"])


if __name__ == "__main__":
    unittest.main()
