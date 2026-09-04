#!/usr/bin/env python3
"""Generate the compact changelog resource bundled into the Switch NRO.

CHANGELOG.md remains the human-readable release history.  This resource keeps
the About page's release list in sync without requiring a C++ or localization
edit for every new release.  Localized overrides can still be supplied by the
About page for releases that have translated notes.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


HEADING = re.compile(
    r"^##\s+\[?(?P<version>\d+\.\d+\.\d+)\]?"
    r"(?:\([^)]*\))?\s+\((?P<date>\d{4}-\d{2}-\d{2})\)\s*$"
)


def parse_releases(markdown: str) -> list[dict[str, str]]:
    releases: list[dict[str, object]] = []
    current: dict[str, object] | None = None

    for line in markdown.splitlines():
        match = HEADING.match(line)
        if match:
            current = {
                "version": match.group("version"),
                "date": match.group("date"),
                "description": [],
                "bullets": [],
                "in_sections": False,
            }
            releases.append(current)
            continue

        if current is None:
            continue

        if line.startswith("### "):
            current["in_sections"] = True
            continue

        stripped = line.strip()
        if not stripped:
            continue

        if stripped.startswith(("- ", "* ")):
            bullets = current["bullets"]
            assert isinstance(bullets, list)
            if len(bullets) < 3:
                bullets.append(stripped[2:].strip())
        elif not current["in_sections"]:
            description = current["description"]
            assert isinstance(description, list)
            description.append(stripped)

    result: list[dict[str, str]] = []
    for release in releases:
        description = " ".join(release["description"])
        bullets = release["bullets"]
        assert isinstance(bullets, list)
        notes = description
        if bullets:
            bullet_text = "\n".join(f"• {bullet}" for bullet in bullets)
            notes = f"{notes}\n{bullet_text}" if notes else bullet_text
        if not notes:
            notes = "See GitHub Releases for the full changelog."

        result.append(
            {
                "version": str(release["version"]),
                "date": str(release["date"]),
                "notes": notes[:2400],
            }
        )
    return result


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} CHANGELOG.md OUTPUT.json", file=sys.stderr)
        return 2

    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    releases = parse_releases(source.read_text(encoding="utf-8"))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps({"releases": releases}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
