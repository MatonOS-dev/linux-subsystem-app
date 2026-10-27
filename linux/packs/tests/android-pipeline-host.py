#!/usr/bin/env python3
"""Host check for the exact static mkfs.erofs invocation used on Android."""
import hashlib
import os
import pathlib
import shutil
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
MKFS = pathlib.Path(os.environ.get("MKFS_EROFS", ROOT / "prebuilt/x86_64/matonos-mkfs-erofs"))
FSCK = os.environ.get("FSCK_EROFS", shutil.which("fsck.erofs"))
if not MKFS.is_file() or not os.access(MKFS, os.X_OK):
    raise SystemExit(f"Missing executable MKFS_EROFS: {MKFS}")
if not FSCK:
    raise SystemExit("Set FSCK_EROFS or install erofs-utils")
if b"INTERP" in subprocess.run(["readelf", "-l", str(MKFS)], check=True,
                               stdout=subprocess.PIPE).stdout:
    raise SystemExit("mkfs.erofs must be static for Android")


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


with tempfile.TemporaryDirectory(prefix="matonos-erofs-pipeline-") as temp:
    base = pathlib.Path(temp)
    tree = base / "deployment" / "files"
    (tree / "bin").mkdir(parents=True)
    (tree / "share/applications").mkdir(parents=True)
    exe = tree / "bin/probe"
    exe.write_bytes(b"\x7fELF" + bytes(range(256)) * 512)
    exe.chmod(0o755)
    (tree / "share/applications/probe.desktop").write_text("[Desktop Entry]\nName=Probe\n")
    (tree / "share/probe-link").symlink_to("applications/probe.desktop")

    uuid = "7e0dca53-e197-5e4e-9a34-7b862a3f0d57"
    outputs = [base / "deployment-a.erofs", base / "deployment-b.erofs"]
    elapsed = []
    for image in outputs:
        started = time.perf_counter()
        subprocess.run([str(MKFS), "-zlz4hc,12", "--preserve-mtime", "--all-root",
                        "--mkfs-time", "-T", "0", "-U", uuid,
                        str(image), str(tree)], check=True, stdout=subprocess.DEVNULL)
        elapsed.append((time.perf_counter() - started) * 1000)
        subprocess.run([FSCK, str(image)], check=True, stdout=subprocess.DEVNULL)
    if digest(outputs[0]) != digest(outputs[1]):
        raise SystemExit("Repeated Android-option EROFS builds differ")
    print(f"PASS static mkfs.erofs={MKFS.stat().st_size} bytes; static executable; deterministic SHA-256={digest(outputs[0])}")
    print(f"RESULT deployment_files={sum(p.stat().st_size for p in tree.rglob('*') if p.is_file())} bytes "
          f"erofs_bytes={outputs[0].stat().st_size} mkfs_elapsed_ms="
          f"{elapsed[0]:.3f},{elapsed[1]:.3f} compression=lz4hc")
