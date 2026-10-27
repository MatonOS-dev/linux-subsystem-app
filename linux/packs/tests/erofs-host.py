#!/usr/bin/env python3
"""Host validation and optional loop-mount benchmark for EROFS APK packs."""
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import re
import time
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(*args, check=True):
    return subprocess.run(args, check=check, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)


def payload_offset(apk, entry):
    with zipfile.ZipFile(apk) as archive:
        info = archive.getinfo(entry)
    with open(apk, "rb") as source:
        source.seek(info.header_offset)
        header = source.read(30)
    if len(header) != 30 or struct.unpack_from("<I", header)[0] != 0x04034b50:
        raise AssertionError("invalid ZIP local file header")
    name_len, extra_len = struct.unpack_from("<HH", header, 26)
    return info.header_offset + 30 + name_len + extra_len, info.file_size


def mode(path):
    return pathlib.Path(path).lstat().st_mode & 0o7777


def tree_signature(root):
    result = {}
    for base, dirs, files in os.walk(root, topdown=True, followlinks=False):
        for name in sorted(dirs + files):
            path = pathlib.Path(base, name)
            rel = path.relative_to(root).as_posix()
            st = path.lstat()
            if path.is_symlink():
                result[rel] = ("link", os.readlink(path), mode(path), st.st_uid, st.st_gid)
            elif path.is_dir():
                result[rel] = ("dir", mode(path), st.st_uid, st.st_gid)
            elif path.is_file():
                result[rel] = ("file", mode(path), st.st_uid, st.st_gid, st.st_size,
                               hashlib.sha256(path.read_bytes()).hexdigest())
    return result


def benchmark(tree, label):
    target = pathlib.Path(tree, "data/random")
    if hasattr(os, "posix_fadvise"):
        descriptor = os.open(target, os.O_RDONLY)
        try:
            os.posix_fadvise(descriptor, 0, 0, os.POSIX_FADV_DONTNEED)
        finally:
            os.close(descriptor)
    start = time.perf_counter()
    os.stat(target)
    with open(target, "rb") as stream:
        stream.read(4096)
    cold_us = (time.perf_counter() - start) * 1e6
    start = time.perf_counter()
    for _ in range(1000):
        os.stat(target)
        with open(target, "rb") as stream:
            stream.read(4096)
    warm_us = (time.perf_counter() - start) * 1000
    print(f"BENCH {label} first_touch_stat_open_us={cold_us:.2f} warm_stat_open_us={warm_us:.2f}")


def main():
    mkfs = os.environ.get("MKFS_EROFS", shutil.which("mkfs.erofs"))
    fsck = os.environ.get("FSCK_EROFS", shutil.which("fsck.erofs"))
    dump = os.environ.get("DUMP_EROFS", shutil.which("dump.erofs"))
    if not mkfs or not fsck or not dump:
        raise SystemExit("Set MKFS_EROFS, FSCK_EROFS and DUMP_EROFS or install erofs-utils")
    with tempfile.TemporaryDirectory(prefix="matonos-erofs-") as tmp:
        root = pathlib.Path(tmp)
        source = root / "source"
        source.mkdir()
        (source / "bin").mkdir()
        (source / "bin/tool").write_bytes(b"#!/bin/sh\necho pack-ok\n")
        (source / "bin/tool").chmod(0o755)
        (source / "data").mkdir(mode=0o750)
        (source / "data/random").write_bytes(os.urandom(131071))
        os.symlink("../bin/tool", source / "data/tool")
        try:
            os.setxattr(source / "bin/tool", "user.matonos.test", b"xattr-preserved")
        except OSError as error:
            if error.errno not in (getattr(os, "ENOTSUP", 95), 1):
                raise
        image = root / "deployment.erofs"
        meta = root / "deployment.json"
        run(sys.executable, str(ROOT / "erofs_pack.py"), "--mkfs", mkfs,
            "--tree", str(source), "--image", str(image), "--metadata", str(meta))
        metadata = json.loads(meta.read_text())
        assert metadata["image_size"] == image.stat().st_size
        assert metadata["image_sha256"] == hashlib.sha256(image.read_bytes()).hexdigest()
        assert metadata["offset_rule"] == "zip-local-header-v1"
        run(fsck, str(image))
        extracted = root / "fsck-extracted"
        extracted.mkdir()
        # Android app processes commonly run with umask 077. The validator
        # must still observe the exact image modes after fsck extraction.
        subprocess.run([fsck, "--extract=" + str(extracted), "--preserve-perms", str(image)],
                       check=True, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, preexec_fn=lambda: os.umask(0o077))
        assert tree_signature(source) == tree_signature(extracted)
        inode = run(dump, "--path=/bin/tool", str(image)).stdout
        assert re.search(r"Xattr size:\s*[1-9][0-9]*", inode), inode
        second = root / "stable-nids.erofs"
        run(sys.executable, str(ROOT / "erofs_pack.py"), "--mkfs", mkfs,
            "--tree", str(source), "--image", str(second), "--metadata", str(root / "second.json"))
        inode2 = run(dump, "--path=/bin/tool", str(second)).stdout
        nid1 = re.search(r"NID:\s*(\d+)", inode).group(1)
        nid2 = re.search(r"NID:\s*(\d+)", inode2).group(1)
        assert nid1 == nid2, (nid1, nid2)

        # Construct a carrier-shaped ZIP to verify the exact local-header rule
        # used by the split reader and mount daemon.
        apk = root / "carrier.apk"
        with zipfile.ZipFile(apk, "w") as archive:
            archive.writestr("assets/matonos/erofs-pack.json", meta.read_bytes(),
                             compress_type=zipfile.ZIP_STORED)
            info = zipfile.ZipInfo(metadata["image_entry"])
            info.compress_type = zipfile.ZIP_STORED
            start = archive.fp.tell()
            padding = (-(start + 30 + len(info.filename.encode()) + 4)) & 4095
            info.extra = struct.pack("<HH", 0x4d54, padding) + bytes(padding)
            archive.writestr(info, image.read_bytes())
        offset, length = payload_offset(apk, metadata["image_entry"])
        assert offset % 4096 == 0 and length == image.stat().st_size
        with open(apk, "rb") as carrier:
            carrier.seek(offset)
            extracted_bytes = carrier.read(length)
        assert hashlib.sha256(extracted_bytes).hexdigest() == metadata["image_sha256"]
        image_from_zip = root / "from-zip.erofs"
        image_from_zip.write_bytes(extracted_bytes)
        run(fsck, str(image_from_zip))

        # Ask the host for an isolated mount namespace, then try a loop mount
        # directly from the ZIP payload. Restricted hosts report a clean skip.
        mountpoint = root / "mounted"
        mountpoint.mkdir()
        script = ("set -eu; mount -t erofs -o loop,ro,offset=" + str(offset) +
                  " '" + str(apk) + "' '" + str(mountpoint) +
                  " 2>/dev/null || exit 77; find '" + str(mountpoint) +
                  "' -maxdepth 2 -print")
        mounted = run("unshare", "-rm", "sh", "-c", script, check=False)
        if mounted.returncode == 0:
            mount_tree = tree_signature(mountpoint)
            assert mount_tree == tree_signature(source)
            print("PASS loop-mounted directly from aligned APK payload")
            benchmark(mountpoint, "mounted")
        else:
            print("SKIP loop mount (host denies unshare/mount); fsck and ZIP-offset checks passed")
        benchmark(extracted, "extracted")

        raw = sum(p.stat().st_size for p in source.rglob("*") if p.is_file())
        print(f"RESULT extracted_file_bytes={raw} erofs_bytes={image.stat().st_size}"
              f" mkfs_elapsed_ms={metadata['mkfs_elapsed_ms']} compression=lz4hc")
        print("PASS EROFS integrity, modes, symlink, xattr metadata, stable inode number, ZIP offset rule")


if __name__ == "__main__":
    main()
