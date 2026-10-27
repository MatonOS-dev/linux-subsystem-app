#!/usr/bin/env python3
"""Offline apply_extra-shaped Debian extraction with the shipped bwrap/proxy.
Run tests/musl-host.sh first to prepare the applet aliases, then pass an output
folder outside source projects. This checks extraction, not Android install.
"""
import importlib.util
import io
import os
import pathlib
import shutil
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
# The wrapper gives us a writable noexec tmpfs, matching the Android app-data
# or EROFS backing-mount constraint this test covers.
app_tmp = tempfile.TemporaryDirectory(prefix='maton-apply-extra-',
                                      dir=os.environ['MATON_NOEXEC_DIR'])
app = pathlib.Path(app_tmp.name)/'app'
app.mkdir(parents=True, exist_ok=True)
(app/'.v3-store-root').touch()
app.chmod(0o700)
(app/'extra').mkdir(parents=True, exist_ok=True)
(app/'bin').mkdir(parents=True, exist_ok=True)
(app/'bin/apply_extra').write_bytes(script.read_bytes())
(app/'bin/apply_extra').chmod(0o755)
# Android's app-data and EROFS backing mounts may be noexec even though the
# proxy emulates execve; assert this fixture really has that property.
shutil.copy2('/bin/sh', app/'bin/sh')
(app/'bin/sh').chmod(0o755)
if os.access(app/'bin/sh', os.X_OK):
    raise RuntimeError('MATON_NOEXEC_DIR is not noexec; cannot run the exec regression')
(app/'bin/apply_extra').write_text((app/'bin/apply_extra').read_text().replace(
    '#!/bin/sh', '#!/app/bin/sh'))
bound_file = out/'bound-file'
bound_file.write_text('bind-fd-file\n')
launcher_source = out/'bind-fd-launcher.c'
# Force the Android-denied linkat fallback for both the script and ELF
# interpreter before bwrap opens the deployment fd, as OSTree does.
launcher_source.write_text(r'''#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 7) return 2;
    char src[4096], dst[4102];
    for (int i = 0; i < 2; i++) {
        if (snprintf(src, sizeof(src), "%s/bin/%s", argv[2],
                     i ? "sh" : "apply_extra") >= (int)sizeof(src)) return 5;
        snprintf(dst, sizeof(dst), "%s.link", src);
        if (link(src, dst)) return 5;
    }
    for (int i = 0; i < 4; i++) {
        int fd = open(argv[i+1], O_PATH);
        if (fd < 0 || dup2(fd, 100+i) < 0) return 3;
        close(fd);
    }
    execv(argv[5], argv+5);
    return 4;
}
''')
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
    args = [str(base/'build/link-deny')] + fixture.proxy + (["--trace"] if os.environ.get("COMPAT_TRACE") else []) + [str(launcher), "/usr", str(app), str(extra), str(bound_file), fixture.bwrap, '--unshare-user', '--disable-userns', '--unshare-pid',
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
    # Inspect outside the proxy: these must be emulated names, not native
    # hardlinks (which would hide the loader's raw-open bug).
    for name in ['apply_extra', 'sh']:
        st = (app/'bin'/name).stat()
        assert st.st_size == 0 and st.st_mode & 0o1777 == 0o1600
    if result.returncode: raise RuntimeError(f'apply_extra exit {result.returncode}: see {out}/stderr.txt')
    assert (extra/'browser/browser').read_text() == 'extra-data-extracted\n'
    assert (extra/'browser/browser').stat().st_mode & 0o777 == 0o755
    assert not (extra/'payload.deb').exists()
    print('PASS apply_extra: emulated-hardlink noexec script and interpreter, bind-fd, Debian/xz extraction, rename and cleanup under Flatpak seccomp without /proc')
finally:
    os.close(fd)
    app_tmp.cleanup()
