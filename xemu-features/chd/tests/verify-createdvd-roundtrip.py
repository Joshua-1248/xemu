#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Byte-identity smoke test for a chdman-created Xbox DVD CHD."""
import argparse, hashlib, pathlib, subprocess, tempfile

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(8 * 1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()

p = argparse.ArgumentParser()
p.add_argument('xiso', type=pathlib.Path)
p.add_argument('chd', type=pathlib.Path)
p.add_argument('--chdman', default='chdman')
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='xemu-chd-verify-') as td:
    out = pathlib.Path(td) / 'roundtrip.iso'
    subprocess.run([a.chdman, 'extractdvd', '-i', str(a.chd), '-o', str(out)], check=True)
    src, got = sha256(a.xiso), sha256(out)
    print('source   ', src)
    print('roundtrip', got)
    if src != got:
        raise SystemExit('FAIL: logical DVD stream differs')
print('PASS: exact logical DVD byte identity')
