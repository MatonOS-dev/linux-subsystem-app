#!/usr/bin/env python3
"""Verify a mapped passwd file is visible to host libc under Android's filter."""
import os
import pathlib
import subprocess
import sys
import tempfile

loader, test = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="passwd-map-", dir=pathlib.Path(test).resolve().parents[1] / "build") as temp:
    root = pathlib.Path(temp)
    home = root / "stub-home"
    home.mkdir()
    name = f"matonos{os.getuid()}"
    passwd = root / "passwd"
    passwd.write_text(f"{name}:x:{os.getuid()}:{os.getgid()}:MatonOS Linux:{home}:/system/bin/sh\n")
    result = subprocess.run([loader, "--android-filter", "--map", str(passwd), "/etc/passwd",
                             test, name, str(home)], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=15)
    if result.returncode:
        raise SystemExit(f"passwd map regression failed ({result.returncode})\n{result.stdout}{result.stderr}")
    print(result.stdout, end="")
