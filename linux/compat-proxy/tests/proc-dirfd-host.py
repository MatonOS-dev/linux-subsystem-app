#!/usr/bin/env python3
"""Chromium's proc fd checks after nested bwrap, with inherited Flatpak BPF."""
import os
import pathlib
import subprocess
from importlib.util import spec_from_file_location, module_from_spec
base = pathlib.Path(__file__).resolve().parents[1]
spec = spec_from_file_location('flatpak_fixture', base/'tests/flatpak-bwrap-host.py')
fixture = module_from_spec(spec)
spec.loader.exec_module(fixture)
bwrap = base/'build/musl-host-root/system_ext/priv-app/MatonLinuxSubsystem/lib/x86_64/libmatonos-app-bwrap.so'
payload = base/'build/proc-dirfd'
subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(base/'tests/proc-dirfd.c'), '-o', str(payload)], check=True)
fd = os.memfd_create('proc-dirfd-flatpak-filter', 0)
try:
    os.write(fd, fixture.flatpak_filter_bytes())
    os.lseek(fd, 0, 0)
    command = [os.environ.get('COMPAT_PROXY', str(base/'build/linux-layer-v3')), '--android-filter', str(bwrap),
               '--ro-bind', '/', '/', '--proc', '/proc', '--seccomp', str(fd), '--',
               str(bwrap), '--ro-bind', '/', '/', '--proc', '/proc', '--tmpfs', '/tmp', '--', str(payload)]
    with open(base/'build/proc-dirfd-nested.log', 'wb') as log:
        result = subprocess.run(command, pass_fds=[fd], stdout=subprocess.PIPE, stderr=log, timeout=30)
    assert result.returncode == 0, f'nested bwrap exit {result.returncode}; see build/proc-dirfd-nested.log'
    assert b'PASS proc dirfd' in result.stdout, result.stdout
    print('PASS nested bwrap --proc with inherited Flatpak filter: proc dirfd object checks')
finally:
    os.close(fd)
