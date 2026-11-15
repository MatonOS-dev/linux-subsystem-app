#!/usr/bin/env python3
"""Offline AppStream refresh/read regression using the shipped musl multicall.
Usage: appstream-host.py MATONOS_FLATPAK OUTPUT_DIRECTORY
"""
import gzip
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='appstream-', dir=out) as temp:
    root = Path(temp)
    env = dict(os.environ)
    for key in list(env):
        if key.startswith(('FLATPAK_', 'XDG_', 'DBUS_')):
            del env[key]
    for key, directory in [('HOME', 'home'), ('XDG_DATA_HOME', 'data'),
                           ('XDG_CACHE_HOME', 'cache'), ('XDG_CONFIG_HOME', 'config'),
                           ('XDG_RUNTIME_DIR', 'run')]:
        path = root / directory
        path.mkdir(mode=0o700)
        env[key] = str(path)
    env['FLATPAK_SYSTEM_DIR'] = str(root / 'system')
    env['FLATPAK_SYSTEM_CACHE_DIR'] = str(root / 'system-cache')
    env['LC_ALL'] = 'C'

    def run(tool, *args):
        command = [str(binary), tool, *map(str, args)]
        result = subprocess.run(command, env=env, text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError(f'{command!r}: exit={result.returncode}\n'
                               f'{result.stdout}{result.stderr}')
        return result.stdout

    arch = run('flatpak', '--default-arch').strip()
    repo = root / 'repo'
    catalog = root / 'catalog'
    catalog.mkdir()
    xml = b'''<?xml version="1.0" encoding="UTF-8"?>
<components version="0.14" origin="fixture">
 <component type="desktop-application">
  <id>org.matonos.AppStreamFixture</id>
  <name>MatonOS Catalog Fixture</name>
  <summary>Offline AppStream refresh regression</summary>
  <description><p>Local metadata for the MatonOS host regression.</p></description>
  <metadata_license>CC0-1.0</metadata_license>
  <bundle type="flatpak">app/org.matonos.AppStreamFixture/ARCH/stable</bundle>
  <releases><release version="1.0" timestamp="1767225600"/></releases>
 </component>
</components>
'''.replace(b'ARCH', arch.encode())
    (catalog / 'appstream.xml.gz').write_bytes(gzip.compress(xml, mtime=0))
    run('ostree', 'init', f'--repo={repo}', '--mode=archive-z2')
    run('ostree', 'commit', f'--repo={repo}', f'--branch=appstream2/{arch}',
        f'--tree=dir={catalog}', '--subject=AppStream fixture')
    run('ostree', 'summary', f'--repo={repo}', '--update')
    run('flatpak', 'remote-add', '--user', '--no-gpg-verify', 'fixture', repo.as_uri())
    run('flatpak', 'update', '--user', '--appstream', 'fixture')
    active = root / 'data/flatpak/appstream/fixture' / arch / 'active'
    assert active.is_dir(), f'AppStream active checkout missing: {active}'
    assert gzip.decompress((active / 'appstream.xml.gz').read_bytes()) == xml
    result = run('flatpak', 'search', '--user', '--columns=application,name,version,remotes',
                 'AppStreamFixture')
    assert 'org.matonos.AppStreamFixture' in result, result
    assert 'MatonOS Catalog Fixture' in result and 'fixture' in result, result
    # Repeated refresh must also preserve the readable checkout.
    run('flatpak', 'update', '--user', '--appstream', 'fixture')
    assert gzip.decompress((active / 'appstream.xml.gz').read_bytes()) == xml
    print('PASS musl Flatpak local AppStream refresh, active checkout and libappstream search')
