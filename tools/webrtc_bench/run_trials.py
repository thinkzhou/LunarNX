#!/usr/bin/env python3
"""Run isolated native receivers against one common native SRTP sender."""
import argparse
import hashlib
import json
import os
import platform
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def revision(path):
    return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()


def fingerprint(paths):
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(str(path.relative_to(ROOT)).encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


def trial(args, backend, rate, repeat):
    directory = args.output / f'{backend}-{rate}mbps-{repeat}'
    directory.mkdir(parents=True, exist_ok=False)
    common = [str(directory), str(rate), str(args.seconds), str(args.idle_us)]
    receiver = sender = None
    environment = dict(os.environ, BENCH_BIND_ADDRESS=args.bind_address)
    with (directory / 'receiver.log').open('w') as rx_log, (directory / 'sender.log').open('w') as tx_log:
        try:
            receiver = subprocess.Popen([str(args.bin / f'{backend}_bench'), 'receiver', *common], stdout=rx_log, stderr=subprocess.STDOUT, env=environment)
            sender = subprocess.Popen([str(args.bin / 'datachannel_bench'), 'sender', *common], stdout=tx_log, stderr=subprocess.STDOUT, env=environment)
            sender_code = sender.wait(timeout=args.seconds + 40)
            receiver_code = receiver.wait(timeout=10)
            if sender_code or receiver_code:
                return {'backend': backend, 'rate_mbps': rate, 'repeat': repeat, 'error': f'exit sender={sender_code} receiver={receiver_code}', 'directory': str(directory)}
            result = json.loads((directory / 'receiver.json').read_text())
            sent = json.loads((directory / 'sender.json').read_text())
            result.update(sent)
            result.update(backend=backend, rate_mbps=rate, repeat=repeat, directory=str(directory))
            result['loss_percent'] = (sent['sent'] - result['received']) / sent['sent'] * 100
            if not result['received'] or result['received'] > sent['sent'] or result['invalid'] or result['duplicates'] or sent['failed_sends']:
                result['error'] = 'invalid delivery sample; inspect counts and logs'
            return result
        except subprocess.TimeoutExpired:
            return {'backend': backend, 'rate_mbps': rate, 'repeat': repeat, 'error': 'process timeout', 'directory': str(directory)}
        finally:
            for process in (receiver, sender):
                if process and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin', type=Path, default=ROOT / 'build/webrtc_bench/harness')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/webrtc_bench' / time.strftime('trials-%Y%m%d-%H%M%S'))
    parser.add_argument('--rates', type=int, nargs='+', default=[15, 30, 50])
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--seconds', type=float, default=6)
    parser.add_argument('--idle-us', type=int, default=1000)
    parser.add_argument('--bind-address', default=subprocess.check_output(['ipconfig', 'getifaddr', 'en0'], text=True).strip())
    args = parser.parse_args()
    if args.seconds <= 0 or args.repeats <= 0 or args.idle_us < 0 or any(rate <= 0 for rate in args.rates):
        parser.error('rates, seconds, and repeats must be positive; idle-us must be nonnegative')
    args.output = args.output.resolve()
    args.bin = args.bin.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {'platform': platform.platform(), 'machine': platform.machine(),
              'libpeer_revision': revision(ROOT / 'lib/libpeer'),
              'libpeer_dirty': bool(subprocess.check_output(['git', '-C', str(ROOT / 'lib/libpeer'), 'status', '--porcelain'], text=True)),
              'libdatachannel_revision': revision(ROOT / 'github_repos/libdatachannel'),
              'libpeer_source_fingerprint': fingerprint(list((ROOT / 'lib/libpeer/src').glob('*.[ch]')) + list((ROOT / 'lib/libpeer/include').glob('*.h'))),
              'shared_srtp_archive_sha256': hashlib.sha256((ROOT / 'lib/libpeer/third_party/libsrtp/build_host/libsrtp2.a').read_bytes()).hexdigest(),
              'srtp_profile': 'SRTP_AES128_CM_SHA1_80',
              'dtls_backends': {'libpeer': 'native mbedTLS', 'datachannel': 'native OpenSSL'},
              'seconds': args.seconds, 'idle_us': args.idle_us, 'bind_address': args.bind_address, 'trials': []}
    for repeat in range(args.repeats):
        for rate in args.rates:
            order = ['libpeer', 'datachannel'] if repeat % 2 == 0 else ['datachannel', 'libpeer']
            for backend in order:
                result = trial(args, backend, rate, repeat)
                report['trials'].append(result)
                (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
                print(json.dumps(result), flush=True)
    return int(any('error' in result for result in report['trials']))


if __name__ == '__main__':
    raise SystemExit(main())
