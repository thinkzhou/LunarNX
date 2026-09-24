#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys
import tempfile
root = Path(__file__).resolve().parents[1]
flags = []
if sys.platform == 'darwin':
    sdk = subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
    flags = ['-isysroot',sdk,'-isystem',sdk+'/usr/include/c++/v1']
with tempfile.TemporaryDirectory(prefix='steam-pointer-test-') as directory:
    exe = str(Path(directory)/'test')
    subprocess.run(['clang++',*flags,'-std=c++20','-fsanitize=address,undefined',
                    '-g','-Isrc','tests/steam_pointer_test.cpp','-o',exe],cwd=root,check=True)
    subprocess.run([exe],check=True)
