#!/usr/bin/env python3
"""A mapped X11 directory with a named socket bound by stock bwrap."""
import pathlib, socket, subprocess, tempfile
base = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='steam-personality-socket-') as directory:
    endpoint = socket.socket(socket.AF_UNIX)
    endpoint.bind(directory + '/X0')
    command = [str(base/'build/linux-layer-v3'), '--android-filter', '--map', str(base/'build/musl-host-root/system_ext'), '/system_ext', '--map', directory,
               '/tmp/.X11-unix', '/system_ext/priv-app/MatonLinuxSubsystem/lib/x86_64/bwrap', '--ro-bind', '/', '/',
               '--tmpfs', '/tmp', '--dir', '/tmp/.X11-unix', '--bind',
               '/tmp/.X11-unix/X0', '/tmp/.X11-unix/X0', '/usr/bin/test', '-S', '/tmp/.X11-unix/X0']
    result = subprocess.run(command, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr.decode(errors='replace')
print('PASS mapped named Unix socket through stock bwrap bind')

# Flatpak's nested launch reuses the outer /dev/shm mount, creates a private
# subdirectory, and binds that through bwrap's oldroot view after pivot_root.
with tempfile.TemporaryDirectory(prefix='steam-personality-nested-') as directory:
    endpoint = socket.socket(socket.AF_UNIX)
    endpoint.bind(directory + '/X0')
    tool = '/mnt/bwrap'
    script = '''set -eu
mkdir -p /dev/shm/outer/inner
printf nested-shm > /dev/shm/outer/inner/marker
exec "$1" --ro-bind / / --ro-bind /mnt /mnt --tmpfs /tmp --dir /tmp/.X11-unix --bind /tmp/.X11-unix/X0 /tmp/.X11-unix/X0 --bind /dev/shm/outer /dev/shm /bin/sh -ec '
exec "$1" --ro-bind / / --ro-bind /mnt /mnt --tmpfs /tmp --dir /tmp/.X11-unix --bind /tmp/.X11-unix/X0 /tmp/.X11-unix/X0 --bind /dev/shm/inner /dev/shm /bin/sh -ec "test -S /tmp/.X11-unix/X0; grep -qx nested-shm /dev/shm/marker"' sh "$1"
'''
    result = subprocess.run([str(base/'build/linux-layer-v3'), '--android-filter',
        '--map', str(base/'build/musl-host-root/system_ext/priv-app/MatonLinuxSubsystem/lib/x86_64'), '/mnt',
        '--map', directory, '/tmp/.X11-unix', '/bin/sh', '-ec', script, 'sh', tool],
        capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr.decode(errors='replace')[-6000:]
print('PASS nested stock bwrap socket and stacked shared-memory binds')
