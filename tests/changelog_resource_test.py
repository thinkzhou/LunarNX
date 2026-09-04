#!/usr/bin/env python3
import json
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/generate_changelog_resource.py"


def generate(source: str) -> list[dict[str, str]]:
    with tempfile.TemporaryDirectory() as directory:
        source_path = Path(directory) / "CHANGELOG.md"
        output_path = Path(directory) / "changelog.json"
        source_path.write_text(source, encoding="utf-8")
        subprocess.run(
            ["python3", str(SCRIPT), str(source_path), str(output_path)],
            check=True,
            cwd=ROOT,
        )
        return json.loads(output_path.read_text(encoding="utf-8"))["releases"]


current = generate((ROOT / "CHANGELOG.md").read_text(encoding="utf-8"))
assert current, "current changelog must produce at least one release"
assert current[0]["version"] == (ROOT / "version.txt").read_text().strip()
assert re.fullmatch(r"\d{4}-\d{2}-\d{2}", current[0]["date"])
assert current[0]["notes"], "current release must have generated notes"

future = generate(
    """# Changelog

## [0.4.0](https://example.invalid) (2026-10-01)

Future release summary.

### Features

- First future feature.
- Second future feature.

## 0.3.0 (2026-09-02)

Older release.
"""
)
assert [release["version"] for release in future] == ["0.4.0", "0.3.0"]
assert "First future feature" in future[0]["notes"]

release_please = generate(
    """# Changelog

## [0.4.0](https://example.invalid) (2026-10-01)

### Features

* **ui:** Future release feature ([abc123](https://example.invalid/abc123))
"""
)
assert "Future release feature" in release_please[0]["notes"]
assert "**" not in release_please[0]["notes"]
assert "https://" not in release_please[0]["notes"]

print("Changelog resource generation tests passed")
