#!/usr/bin/env python3
"""Fetch hash-locked upstream sources and MPL-licensed Switch adaptations."""
import concurrent.futures
import hashlib
import io
import json
import re
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'build/libdatachannel-sdk'
DONOR_REV = 'd9b640f320227dcd3857c6ad2947234d39a0a2b3'
DONOR_SHA = '66c83fe531db84eb048d8ad0880904efc67cb9cc26e5395b41507a55d253be14'


def extract(repo, ref, checksum, destination, archive_name):
    archive = BASE / 'cache' / archive_name
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        with urllib.request.urlopen(f'https://codeload.github.com/{repo}/tar.gz/{ref}', timeout=90) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != checksum:
            raise ValueError('source archive hash mismatch: ' + repo)
        archive.write_bytes(data)
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != checksum:
        raise ValueError('cached archive hash mismatch: ' + repo)
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as source:
        for member in source:
            parts = Path(member.name).parts[1:]
            if not parts:
                continue
            target = destination.joinpath(*parts)
            if '..' in parts or Path(*parts).is_absolute() or not target.resolve().is_relative_to(destination.resolve()):
                raise ValueError('unsafe archive member')
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(source.extractfile(member).read())
            else:
                raise ValueError('unexpected archive link or special file')


def prepare():
    marker = BASE / 'source/prepared.sha256'
    identity = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    if marker.exists() and marker.read_text() == identity:
        return
    donor = BASE / 'reference'
    extract('doke69/YOUYOU-Remote-ns', DONOR_REV, DONOR_SHA, donor, 'youyou.tar.gz')
    lock = json.loads((donor / 'scripts/sources.lock.json').read_text())
    selected = [x for x in lock if x['name'] in ('libdatachannel', 'libjuice', 'usrsctp', 'plog')]
    # Extract the parent before nested dependencies.
    for entry in selected:
        extract(entry['repo'], entry['ref'], entry['sha256'],
                BASE / 'source' / entry['path'], entry['name'] + '.tar.gz')
    overlay = donor / 'scripts/dependency-overrides'
    for entry in json.loads((overlay / 'manifest.json').read_text()):
        if entry['dependency'] == 'libsrtp':
            continue  # Reuse LunarNX's single libSRTP ABI and crypto backend.
        path = entry['path']
        source = overlay / path
        destination = BASE / 'source/libdatachannel' / path
        data = source.read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise ValueError('adaptation hash mismatch: ' + path)
        current = destination.read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(current).hexdigest() != entry['upstream_sha256']:
            raise ValueError('upstream source hash mismatch: ' + path)
        text = data.decode()
        # Application diagnostics are not part of the protocol adaptation.
        text = re.sub(r'\buu_cpu_mark\([05]\);', '', text)
        text = re.sub(r'uu::diagnosticSctpHandshake\([^;]*\);', '', text)
        text = text.replace('#include "rtc_diagnostics.hpp"', '')
        # Own the cleanup threads in LunarNX rather than importing UU app code.
        text = text.replace('uu::backgroundTask(', 'lunar::webrtc::backgroundTask(')
        destination.write_text(text)
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(identity)
    print('Prepared pinned libdatachannel / libjuice sources and Switch adaptations')


if __name__ == '__main__':
    prepare()
