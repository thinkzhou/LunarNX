#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLISHER = ROOT / "tools/dev_bridge/publish_build.sh"


def main() -> None:
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        nro = temp / "test.nro"
        nro.write_bytes(b"not-a-release-build")
        env = os.environ.copy()
        env["LUNARNX_DEV_BRIDGE_URL"] = "https://bridge.invalid"
        env["LUNARNX_BUILD_MANIFEST"] = str(temp / "missing-manifest.json")
        result = subprocess.run(
            [str(PUBLISHER), "--no-feishu", str(nro), "test-version"],
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert result.returncode == 10, result.stderr
        assert "token-verified build manifest" in result.stderr

    print("Development bridge publish guard test passed")


if __name__ == "__main__":
    main()
