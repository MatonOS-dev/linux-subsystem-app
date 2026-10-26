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
script.write_text('#!/bin/sh\nset -eu\ntest ! -e /proc/self\ntest \"$(cat /etc/bind-fd-fixture)\" = bind-fd-file\nbsdtar -xf payload.deb data.tar.xz\nbsdtar -xf data.tar.xz\nmv opt/browser browser\nrm -r opt payload.deb data.tar.xz\n')
script.chmod(0o755)
app = out/'app'
(app/'extra').mkdir(parents=True, exist_ok=True)
(app/'bin').mkdir(parents=True, exist_ok=True)
(app/'bin/apply_extra').write_bytes(script.read_bytes())
(app/'bin/apply_extra').chmod(0o755)
bound_file = out/'bound-file'
bound_file.write_text('bind-fd-file\n')
launcher_source = out/'bind-fd-launcher.c'
launcher_source.write_text('#define _GNU_SOURCE\n#include <fcntl.h>\n#include <unistd.h>\nint main(int argc,char **argv){if(argc<7)return 2;for(int i=0;i<4;i++){int fd=open(argv[i+1],O_PATH);if(fd<0||dup2(fd,100+i)<0)return 3;close(fd);}execv(argv[5],argv+5);return 4;}\n')
launcher = out/'bind-fd-launcher'
subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(launcher_source), '-o', str(launcher)], check=True)
# Open inside the proxy so fd tracking matches Flatpak; include a regular-file
# bind because directory-only tests miss bwrap's file placeholder/reopen path.
mount_fds = [100, 101, 102, 103]
# Android's /data/user/0 alias canonicalizes to /data/data in proc fd links.
# A symlinked ancestor models the same mismatch without an Android device.
tmp_alias = out/'tmp-alias'
if not tmp_alias.exists(): tmp_alias.symlink_to(out, target_is_directory=True)
(out/'tmp').mkdir(exist_ok=True)
fd = os.memfd_create('apply-extra-flatpak-filter', 0)
try:
    os.write(fd, fixture.flatpak_filter_bytes())
    os.lseek(fd, 0, 0)
    args = fixture.proxy + (["--trace"] if os.environ.get("COMPAT_TRACE") else []) + [str(launcher), "/usr", str(app), str(extra), str(bound_file), fixture.bwrap, '--unshare-user', '--unshare-pid',
        '--unshare-net', '--unshare-ipc', '--cap-drop', 'ALL',
        '--ro-bind-fd', str(mount_fds[0]), '/usr', '--symlink', 'usr/bin', '/bin',
        '--symlink', 'usr/lib', '/lib', '--symlink', 'usr/lib64', '/lib64',
        '--ro-bind-fd', str(mount_fds[3]), '/etc/bind-fd-fixture',
        '--tmpfs', '/tmp', '--dir', '/var/tmp', '--dev', '/dev',
        '--ro-bind-fd', str(mount_fds[1]), '/app',
        '--bind-fd', str(mount_fds[2]), '/app/extra', '--chdir', '/app/extra',
        '--setenv', 'PATH', '/usr/bin:/bin', '--seccomp', str(fd),
        '--', '/app/bin/apply_extra']
    with (out/'stdout.txt').open('wb') as stdout, (out/'stderr.txt').open('wb') as stderr:
        result = subprocess.run(args, pass_fds=[fd], stdout=stdout, stderr=stderr,
            env={'PATH': '/usr/bin:/bin', 'TMPDIR': str(tmp_alias/'tmp')}, timeout=40)
    if result.returncode: raise RuntimeError(f'apply_extra exit {result.returncode}: see {out}/stderr.txt')
    assert (extra/'browser/browser').read_text() == 'extra-data-extracted\n'
    assert (extra/'browser/browser').stat().st_mode & 0o777 == 0o755
    assert not (extra/'payload.deb').exists()
    print('PASS apply_extra: bsdtar Debian/xz extraction, rename and cleanup under Flatpak seccomp without /proc')
finally:
    os.close(fd)
