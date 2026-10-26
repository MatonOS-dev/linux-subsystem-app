#!/usr/bin/env python3
"""Offline apply_extra-shaped Debian extraction with the shipped bwrap/proxy.
Run tests/musl-host.sh first to prepare the applet aliases, then pass an output
folder outside source projects. This checks extraction, not Android install.
"""
import importlib.util
import io
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile

base = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('bwrap_fixture', base/'tests/flatpak-bwrap-host.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
out = pathlib.Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
extra = pathlib.Path(tempfile.mkdtemp(prefix='extra-', dir=out))
# Debian ar container with an xz-compressed data member, as apply_extra uses.
data = io.BytesIO()
with tarfile.open(fileobj=data, mode='w:xz') as archive:
    body = b'extra-data-extracted\n'
    entry = tarfile.TarInfo('opt/browser/browser')
    entry.size = len(body)
    entry.mode = 0o755
    archive.addfile(entry, io.BytesIO(body))
with (extra/'payload.deb').open('wb') as deb:
    deb.write(b'!<arch>\n')
    for name, body in [('debian-binary', b'2.0\n'), ('data.tar.xz', data.getvalue())]:
        header = f'{name+"/":<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{len(body):<10}`\n'
        deb.write(header.encode('ascii'))
        deb.write(body)
        if len(body)%2: deb.write(b'\n')
script = out/'apply_extra'
script.write_text('#!/bin/sh\nset -eu\ntest ! -e /proc/self\nbsdtar -xf payload.deb data.tar.xz\nbsdtar -xf data.tar.xz\nmv opt/browser browser\nrm -r opt payload.deb data.tar.xz\n')
script.chmod(0o755)
fd = os.memfd_create('apply-extra-flatpak-filter', 0)
try:
    os.write(fd, fixture.flatpak_filter_bytes())
    os.lseek(fd, 0, 0)
    args = fixture.proxy + (["--trace"] if os.environ.get("COMPAT_TRACE") else []) + [fixture.bwrap, '--unshare-user', '--unshare-pid',
        '--unshare-net', '--unshare-ipc', '--cap-drop', 'ALL',
        '--ro-bind', '/usr', '/usr', '--symlink', 'usr/bin', '/bin',
        '--symlink', 'usr/lib', '/lib', '--symlink', 'usr/lib64', '/lib64',
        '--tmpfs', '/tmp', '--dir', '/var/tmp', '--dev', '/dev',
        '--ro-bind', str(script), '/app/bin/apply_extra',
        '--bind', str(extra), '/app/extra', '--chdir', '/app/extra',
        '--setenv', 'PATH', '/usr/bin:/bin', '--seccomp', str(fd),
        '--', '/app/bin/apply_extra']
    with (out/'stdout.txt').open('wb') as stdout, (out/'stderr.txt').open('wb') as stderr:
        result = subprocess.run(args, pass_fds=[fd], stdout=stdout, stderr=stderr,
            env={'PATH': '/usr/bin:/bin', 'TMPDIR': str(out)}, timeout=40)
    if result.returncode: raise RuntimeError(f'apply_extra exit {result.returncode}: see {out}/stderr.txt')
    assert (extra/'browser/browser').read_text() == 'extra-data-extracted\n'
    assert (extra/'browser/browser').stat().st_mode & 0o777 == 0o755
    assert not (extra/'payload.deb').exists()
    print('PASS apply_extra: bsdtar Debian/xz extraction, rename and cleanup under Flatpak seccomp without /proc')
finally:
    os.close(fd)
