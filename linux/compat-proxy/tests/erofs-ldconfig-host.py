#!/usr/bin/env python3
"""Replay Flatpak's EROFS ldconfig phase from captured APK splits.

Set COMPAT_EROFS_LDCONFIG_DATA to the directory containing the captured
package directories and FSCK_EROFS to an erofs-utils fsck.erofs binary. The
device capture is optional, so the regular host suite skips when it is absent.
"""
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

BASE = pathlib.Path(__file__).resolve().parents[1]
DATA_VALUE = os.environ.get("COMPAT_EROFS_LDCONFIG_DATA")
DATA = pathlib.Path(DATA_VALUE) if DATA_VALUE else None
FSCK = pathlib.Path(os.environ.get("FSCK_EROFS", "fsck.erofs"))
PROXY = BASE / "build/linux-layer-v3"
PACK = "assets/matonos/pack.erofs"
INDEX = "assets/matonos/layers.index"

PACKAGE_PREFIXES = [
    "flatpak.app.net_sourceforge_ExtremeTuxRacer",
    "flatpak.runtime.org_freedesktop_Platform.x86_64",
    "flatpak.runtime.org_freedesktop_Platform_GL_default.x86_64.v25_08.",
    "flatpak.runtime.org_freedesktop_Platform_GL_default.x86_64.v25_08_extra.",
    "flatpak.runtime.org_freedesktop_Platform_Locale.x86_64",
    "flatpak.runtime.org_freedesktop_Platform_codecs_extra.x86_64",
]


def string(data, offset):
    if offset + 4 > len(data):
        raise ValueError("truncated layers.index string length")
    size = struct.unpack_from("<I", data, offset)[0]
    offset += 4
    if offset + size > len(data):
        raise ValueError("truncated layers.index string")
    return data[offset:offset + size].decode("utf-8"), offset + size


def layer_splits(index):
    if index[:8] != b"MTPKIDX2":
        raise ValueError("expected MTPKIDX2 index")
    count = struct.unpack_from("<I", index, 8)[0]
    offset = 12
    rows = []
    for _ in range(count):
        split, offset = string(index, offset)
        entry, offset = string(index, offset)
        if offset + 40 > len(index):
            raise ValueError("truncated layers.index descriptor")
        size = struct.unpack_from("<Q", index, offset)[0]
        digest = index[offset + 8:offset + 40].hex()
        offset += 40
        if split != "pack_" + digest or entry != PACK or not size:
            raise ValueError(f"invalid layer descriptor {split}")
        rows.append(split)
    return rows


def find_package(prefix):
    matches = sorted(path for path in DATA.iterdir() if path.is_dir() and path.name.startswith(prefix))
    if len(matches) != 1:
        raise RuntimeError(f"expected one captured package starting {prefix!r}, found {len(matches)}")
    return matches[0]


def java_utf(data, offset):
    if offset + 2 > len(data):
        raise ValueError("truncated deployment UTF length")
    size = struct.unpack_from(">H", data, offset)[0]
    offset += 2
    if offset + size > len(data):
        raise ValueError("truncated deployment UTF value")
    return data[offset:offset + size].decode("utf-8"), offset + size


def deployment(data):
    version, offset = java_utf(data, 0)
    if version != "MTDEP3":
        raise ValueError(f"unexpected deployment record {version!r}")
    ref, offset = java_utf(data, offset)
    commit, offset = java_utf(data, offset)
    remote_size = struct.unpack_from(">I", data, offset)[0]
    offset += 4 + remote_size
    mount, offset = java_utf(data, offset)
    for _ in range(3):
        size = struct.unpack_from(">I", data, offset)[0]
        offset += 4 + size
    count = struct.unpack_from(">I", data, offset)[0]
    offset += 4
    dependencies = []
    for _ in range(count):
        dep_ref, offset = java_utf(data, offset)
        dep_mount, offset = java_utf(data, offset)
        size = struct.unpack_from(">I", data, offset)[0]
        offset += 4 + size
        dependencies.append((dep_ref, dep_mount))
    if offset != len(data):
        raise ValueError("trailing deployment record bytes")
    return ref, commit, mount, dependencies


def package_deployment(package):
    candidates = sorted(package.glob("split_index_*.apk"))
    if len(candidates) != 1:
        raise RuntimeError(f"expected one deployment index split in {package}")
    with zipfile.ZipFile(candidates[0]) as apk:
        return deployment(apk.read("assets/matonos/deployment.bin"))


def main():
    if DATA is None or not DATA.is_dir():
        print("SKIP EROFS ldconfig capture absent (set COMPAT_EROFS_LDCONFIG_DATA)")
        return 0
    if not PROXY.is_file():
        raise RuntimeError("build the compat proxy first")
    if isinstance(FSCK, pathlib.Path) and "/" in str(FSCK) and not FSCK.is_file():
        raise RuntimeError(f"fsck.erofs not found: {FSCK}")

    # Scratch is optional for inspection after a failing reproduction.
    keep = os.environ.get("COMPAT_EROFS_LDCONFIG_KEEP")
    scratch = pathlib.Path(keep) if keep else pathlib.Path(tempfile.mkdtemp(prefix="erofs-ldconfig-"))
    scratch.mkdir(parents=True, exist_ok=True)
    try:
        root = scratch / "root"
        root.mkdir(exist_ok=True)
        packages = [find_package(prefix) for prefix in PACKAGE_PREFIXES]
        records = {package.name: package_deployment(package) for package in packages}
        mounts = {records[packages[0].name][0]: records[packages[0].name][2]}
        for _, _, _, dependencies in records.values():
            for ref, target in dependencies:
                prior = mounts.setdefault(ref, target)
                if prior != target:
                    raise RuntimeError(f"conflicting declared mounts for {ref}: {prior} / {target}")
        args = [str(PROXY), "--android-filter", "--root", str(root)]
        ldconfig = None
        runtime_layers = []
        for package_no, package in enumerate(packages):
            ref, commit, _, _ = records[package.name]
            target = mounts.get(ref)
            if target is None:
                raise RuntimeError(f"no declared mount found for {ref}")
            base_apk = package / "base.apk"
            with zipfile.ZipFile(base_apk) as apk:
                index = apk.read(INDEX)
            table = scratch / f"table-{package_no}"
            table.mkdir(exist_ok=True)
            (table / "layers.index").write_bytes(index)
            with zipfile.ZipFile(base_apk) as apk:
                split_names = layer_splits(index)
                for split in split_names:
                    carrier = package / f"split_{split}.apk"
                    if not carrier.is_file():
                        raise RuntimeError(f"missing captured split {carrier.name} in {package}")
                    with zipfile.ZipFile(carrier) as split_apk:
                        image = split_apk.read(PACK)
                    layer = scratch / "layers" / f"{package_no}-{split[5:]}"
                    if not layer.exists():
                        layer.mkdir(parents=True)
                        image_path = scratch / f"{package_no}-{split}.erofs"
                        image_path.write_bytes(image)
                        subprocess.run([str(FSCK), f"--extract={layer}", str(image_path)],
                                       check=True, stdout=subprocess.DEVNULL)
                        image_path.unlink()
                    if not (table / split).exists():
                        (table / split).symlink_to(layer)
                    if package_no == 1:
                        runtime_layers.append(layer)
            args += ["--erofs-mount", target, str(table / "layers.index"), str(table)]
            backing = root / "flatpak-launch" / ("app" if package_no == 0 else "system")
            backing = backing / ref / commit / "files"
            backing.mkdir(parents=True, exist_ok=True)
            args += ["--erofs-backing", target, "/" + str(backing.relative_to(root))]
        ldconfig_candidates = [layer / "usr/bin/ldconfig" for layer in runtime_layers]
        ldconfig_candidates += [layer / "bin/ldconfig" for layer in runtime_layers]
        ldconfig = next((path for path in ldconfig_candidates if path.is_file()), None)

        if ldconfig is None or not ldconfig.is_file():
            raise RuntimeError("Platform runtime EROFS does not contain usr/bin/ldconfig")
        (root / "etc").mkdir(exist_ok=True)
        (root / "etc/ld.so.conf").write_text(
            "/app/lib\n/usr/lib/x86_64-linux-gnu/GL/maton/lib\n"
            "/usr/lib/x86_64-linux-gnu/GL/default/lib\n"
        )
        cache = root / "run/ld-so-cache-dir"
        cache.mkdir(parents=True, exist_ok=True)

        # Flatpak's ldconfig phase runs in the sandbox root, with this same
        # command shape and no host ld.so.cache bind.
        command = args + ["/usr/bin/ldconfig", "-X", "-C", "/run/ld-so-cache-dir/cache"]
        trace = scratch / "strace.log"
        debugger = os.environ.get("COMPAT_EROFS_LDCONFIG_GDB")
        if debugger:
            command = [debugger, "--batch", "--quiet", "-ex", "set pagination off",
                       "-ex", "handle SIGABRT stop print pass", "-ex", "run", "-ex", "bt",
                       "--args"] + command
        elif shutil.which("strace"):
            command = ["strace", "-f", "-o", str(trace), "-e", "trace=%file,%memory,signal"] + command
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=180)
        (scratch / "stdout.log").write_text(result.stdout)
        (scratch / "stderr.log").write_text(result.stderr)
        print(f"ldconfig exit={result.returncode}; scratch={scratch}")
        if result.stdout:
            print(result.stdout[-4000:])
        if result.stderr:
            print(result.stderr[-8000:], file=sys.stderr)
        if trace.exists():
            print(f"strace={trace}")
        if result.returncode:
            return result.returncode if result.returncode > 0 else 128 - result.returncode
        if not (cache / "cache").is_file():
            raise RuntimeError("ldconfig exited successfully but did not write its cache")
        print("PASS captured Flatpak EROFS ldconfig")
        return 0
    finally:
        if not keep:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL EROFS ldconfig host repro: {error}", file=sys.stderr)
        sys.exit(1)
