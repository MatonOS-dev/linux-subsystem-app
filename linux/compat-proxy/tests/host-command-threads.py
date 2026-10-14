#!/usr/bin/env python3
"""File traffic in worker SIGSYS handlers after first Flatpak resident exec.

Use the real snapshot/worker path with an inherited socketpair: no command is
sent, and the payload never uses HostCommand. Also accepts an NDK compiler to
exercise Android code generation on the host without an image or adb.
"""
import argparse
import os
import pathlib
import socket
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--android-compiler')
args = parser.parse_args()
compiler = args.android_compiler or 'cc'
loader = ROOT / 'build/host-command-threads-loader'
flags = ['-Wl,-no-pie', '-Wl,--image-base=0x600000000000'] if args.android_compiler else ['-no-pie', '-Wl,-Ttext-segment=0x600000000000']
subprocess.run([compiler, '-DHOST_COMMAND_TEST', '-nostdlib', '-static',
                '-mcmodel=large', *flags, '-O2', '-fno-stack-protector',
                '-fno-builtin', '-Wall', '-Wextra', '-Werror',
                *[str(ROOT / s) for s in ['loader.c', 'proxy.c', 'vfs.c',
                                           'links.c', 'net.c', 'sysvipc.c', 'raw.S', 'entry.S']],
                '-o', str(loader)], check=True)
with tempfile.TemporaryDirectory(prefix='hc-threads-', dir='/mnt/data/aosp/out') as tmp:
    d = pathlib.Path(tmp)
    app = d / 'app'
    app.mkdir()
    info = d / 'info'
    info.write_text('[Application]\nname=worker-regression\n')
    subprocess.run(['cc', '-pthread', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'tests/host-command-threads.c'), '-o', str(app / 'test')], check=True)
    client, worker = socket.socketpair()
    # bwrap's sync fd is protected until the first /.flatpak-info exec. The
    # snapshot then forks its resident worker and closes the payload copy.
    try:
        command = [str(loader), '--android-filter', '--host-command-fd',
                   str(worker.fileno()), '/usr/bin/bwrap', '--sync-fd',
                   str(worker.fileno()), '--tmpfs', '/', '--ro-bind', '/usr', '/usr',
                   '--symlink', 'usr/bin', '/bin', '--symlink', 'usr/lib', '/lib',
                   '--symlink', 'usr/lib64', '/lib64', '--proc', '/proc',
                   '--dev', '/dev', '--bind', str(app), '/app', '--ro-bind',
                   str(info), '/.flatpak-info', '--chdir', '/app', '/app/test']
        result = subprocess.run(command, pass_fds=[worker.fileno()],
                                capture_output=True, timeout=60)
        if result.returncode:
            raise RuntimeError(f'exit {result.returncode}\n{result.stdout.decode()}\n{result.stderr.decode()}')
        # The idle worker rejects its empty request on timeout/runtime death.
        # This proves the /.flatpak-info snapshot actually started; no command
        # has been requested or executed.
        client.settimeout(5)
        reply = client.recv(4, socket.MSG_WAITALL)
        assert reply == struct.pack('<i', -22), f'no resident snapshot worker: {reply!r}'
        print(result.stdout.decode(), end='')
    finally:
        client.close()
        worker.close()
