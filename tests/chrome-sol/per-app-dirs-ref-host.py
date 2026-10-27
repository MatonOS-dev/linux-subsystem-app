#!/usr/bin/env python3
"""Run a Flatpak-shaped command lookup through bwrap and the compat proxy."""
import os
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
runtime = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
proxy = root / "linux/compat-proxy/build/linux-layer-v3"
bwrap = root / "linux/compat-proxy/build/musl-host-root/system_ext/priv-app/MatonLinuxSubsystem/lib/x86_64/bwrap"
marker = runtime / ".flatpak/com.google.Chrome/.ref"
if not marker.is_file() or marker.stat().st_size:
    raise SystemExit(f"missing empty runtime lock file: {marker}")
if not proxy.is_file() or not bwrap.exists():
    raise SystemExit("build compat-proxy/tests/run.sh and linux/compat-proxy/tests/musl-host.sh first")

probe = out / "chrome"
subprocess.run(["cc", "-static", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "tests/chrome-sol/per-app-dirs-ref.c"), "-o", str(probe)], check=True)
probe.chmod(0o755)
command = [
    str(proxy), "--android-filter", "--map", str(runtime), "/run/flatpak",
    str(bwrap), "--tmpfs", "/", "--ro-bind", "/usr", "/usr",
    "--symlink", "usr/bin", "/bin", "--proc", "/proc", "--dev", "/dev",
    "--dir", "/app", "--dir", "/app/bin", "--dir", "/run", "--dir", "/run/flatpak",
    "--ro-bind", "/run/flatpak/.flatpak/com.google.Chrome/.ref", "/run/flatpak/per-app-dirs-ref",
    "--lock-file", "/run/flatpak/per-app-dirs-ref",
    "--ro-bind", str(probe), "/app/bin/chrome",
    "--setenv", "PATH", "/app/bin:/usr/bin", "chrome",
]
result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, timeout=30)
print(result.stdout, end="")
if result.returncode:
    raise SystemExit(f"bwrap Chrome command failed ({result.returncode})")
if "PASS bwrap execvp chrome reads the Flatpak per-app ref lock" not in result.stdout:
    raise SystemExit("Chrome command did not read the per-app ref lock")
