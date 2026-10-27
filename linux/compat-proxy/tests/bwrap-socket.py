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
