#!/usr/bin/env python3
"""Run the real native app against an already-running local mock server."""
import argparse
import json
import os
import re
import signal
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--backend', choices=('libpeer', 'libdatachannel'), required=True)
parser.add_argument('--url', default='http://127.0.0.1:18080')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--seconds', type=int, default=10)
parser.add_argument('--debug-rtc', action='store_true')
args = parser.parse_args()
trial = args.output.resolve()
trial.mkdir(parents=True, exist_ok=False)
(trial / 'config.json').write_text(json.dumps({'xbox_webrtc_backend': args.backend}))
environment = dict(os.environ, LUNARNX_MOCK_URL=args.url)
if args.debug_rtc:
    environment['LUNARNX_RTC_DEBUG'] = '1'
streaming = False
with (trial / 'client.log').open('w') as log:
    process = subprocess.Popen([str(ROOT / 'build/pc/LunarNX_Desktop')], cwd=trial,
                               env=environment, stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 60
        while process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.5)
            if '=== STREAMING ===' in (trial / 'client.log').read_text():
                streaming = True
                time.sleep(args.seconds)
                break
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
text = (trial / 'client.log').read_text()
match = re.search(r'MOCK_METRICS video_frames=(\d+) audio_frames=(\d+) input_packets=(\d+)', text)
metrics = dict(zip(('video_frames', 'audio_frames', 'input_packets'), map(int, match.groups()))) if match else {}
result = {'metrics': metrics, 'backend': args.backend, 'streaming': streaming, 'exit_code': process.returncode,
          'seconds_requested': args.seconds, 'directory': str(trial)}
(trial / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
raise SystemExit(0 if streaming and process.returncode == 0 and metrics and all(value > 0 for value in metrics.values()) else 1)
