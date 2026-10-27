#!/usr/bin/env python3
"""Payload self re-exec $ORIGIN after nested bwrap, with inherited Flatpak BPF."""
import os
import pathlib
import subprocess
import shutil
from importlib.util import spec_from_file_location, module_from_spec
base = pathlib.Path(__file__).resolve().parents[1]
spec = spec_from_file_location('flatpak_fixture', base/'tests/flatpak-bwrap-host.py')
fixture = module_from_spec(spec)
spec.loader.exec_module(fixture)
bwrap = shutil.which('bwrap')
if not bwrap:
    raise RuntimeError('exec-origin nested regression requires bwrap')
payload = '/app/payload'
fd = os.memfd_create('exec-origin-flatpak-filter', 0)
try:
    os.write(fd, fixture.flatpak_filter_bytes())
    os.lseek(fd, 0, 0)
    command = [os.environ.get('COMPAT_PROXY', str(base/'build/linux-layer-v3')), '--android-filter', '--map', str(base/'build/exec-origin'), '/app', str(bwrap),
               '--ro-bind', '/', '/', '--proc', '/proc', '--seccomp', str(fd), '--',
               str(bwrap), '--ro-bind', '/', '/', '--proc', '/proc', '--tmpfs', '/tmp', '--', str(payload)]
    with open(base/'build/exec-origin-nested.log', 'wb') as log:
        result = subprocess.run(command, pass_fds=[fd], stdout=subprocess.PIPE, stderr=log, timeout=30)
    assert result.returncode == 0, f'nested bwrap exit {result.returncode}; see build/exec-origin-nested.log'
    assert b'PASS proc self re-exec' in result.stdout, result.stdout
    print('PASS nested bwrap --proc with inherited Flatpak filter: payload identity and $ORIGIN')
finally:
    os.close(fd)
