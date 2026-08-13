#!/usr/bin/env python3

from pathlib import PurePath
import errno
import json
import os
import shlex
import subprocess
import sys

def destdir_join(d1: str, d2: str) -> str:
    if not d1:
        return d2
    # c:\destdir + c:\prefix must produce c:\destdir\prefix
    return str(PurePath(d1, *PurePath(d2).parts[1:]))

introspect = os.environ.get('MESONINTROSPECT')
if not introspect:
    sys.exit(0)

try:
    out = subprocess.run([*shlex.split(introspect), '--installed'],
                         stdout=subprocess.PIPE, check=True).stdout
    installed = json.loads(out)
except Exception:
    sys.exit(0)

for source, dest in installed.items():
    bundle_dest = destdir_join('qemu-bundle', dest)
    path = os.path.dirname(bundle_dest)
    try:
        os.makedirs(path, exist_ok=True)
    except Exception:
        pass

    try:
        if os.path.lexists(bundle_dest):
            os.remove(bundle_dest)
        os.symlink(source, bundle_dest)
    except Exception:
        pass
