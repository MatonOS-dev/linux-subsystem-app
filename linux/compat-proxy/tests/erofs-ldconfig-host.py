#!/usr/bin/env python3
"""Replay Flatpak's ldconfig phase from captured package EROFS layer APKs.

Set COMPAT_EROFS_LDCONFIG_DATA to the directory containing the captured
package directories and EROFS_FUSE to an erofs-utils erofsfuse binary. The
legacy capture is mounted with FUSE, merged to its package tree, and replayed
through the new one-prefix-mount-per-package proxy path.
"""
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

BASE = pathlib.Path(__file__).resolve().parents[1]
DATA_VALUE = os.environ.get("COMPAT_EROFS_LDCONFIG_DATA")
DATA = pathlib.Path(DATA_VALUE) if DATA_VALUE else None
FUSE = pathlib.Path(os.environ.get("EROFS_FUSE", "erofsfuse"))
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


def deployment_rows(index):
    if index[:8] != b"MTPKIDX2":
        raise ValueError("expected MTPKIDX2 index")
    count = struct.unpack_from("<I", index, 8)[0]
    offset = 12
    layers = []
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
        layers.append(split)
    if offset+4>len(index):raise ValueError("truncated deployment records")
    count=struct.unpack_from("<I",index,offset)[0];offset+=4;rows=[]
    for _ in range(count):
        path,offset=string(index,offset);checksum,offset=string(index,offset)
        kind,offset=string(index,offset);target,offset=string(index,offset)
        if offset+4>len(index):raise ValueError("truncated xattrs")
        size=struct.unpack_from("<I",index,offset)[0];offset+=4+size
        if offset+36>len(index):raise ValueError("truncated deployment metadata")
        layer,file_size,mtime,uid,gid,mode,reserved=struct.unpack_from("<IQQIIII",index,offset);offset+=36
        if reserved or (layer!=0xffffffff and layer>=len(layers)):raise ValueError("invalid deployment layer reference")
        rows.append((path,kind,target,layer,file_size,mode))
    if offset!=len(index):raise ValueError("trailing deployment index bytes")
    return layers,rows


def materialize(rows,layers,dest):
    for path,kind,target,layer,size,mode in sorted(rows,key=lambda row:(row[0].count("/"),row[0])):
        output=dest/path.lstrip("/")
        if kind=="dir":output.mkdir(parents=True,exist_ok=True);continue
        source=layers[layer]/path.lstrip("/")
        output.parent.mkdir(parents=True,exist_ok=True)
        if output.exists() or output.is_symlink():output.unlink()
        if kind=="symlink":output.symlink_to(os.readlink(source))
        else:shutil.copyfile(source,output);output.chmod(mode&0o7777)
    for path,kind,_,_,_,mode in sorted(rows,key=lambda row:len(row[0]),reverse=True):
        if kind=="dir":(dest/path.lstrip("/")).chmod(mode&0o7777)


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
    if "/" in str(FUSE) and not FUSE.is_file():
        raise RuntimeError(f"erofsfuse not found: {FUSE}")

    # Scratch is optional for inspection after a failing reproduction.
    keep = os.environ.get("COMPAT_EROFS_LDCONFIG_KEEP")
    scratch = pathlib.Path(keep) if keep else pathlib.Path(tempfile.mkdtemp(prefix="erofs-ldconfig-"))
    scratch.mkdir(parents=True, exist_ok=True)
    mounted=[]
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
            branch=ref.rsplit("/",1)[-1] if ref.startswith("runtime/") else ""
            if target.endswith("/GL") and ".Platform.GL." in ref:
                target += "/maton" if branch.endswith("-extra") else "/default"
            base_apk = package / "base.apk"
            with zipfile.ZipFile(base_apk) as apk:
                index = apk.read(INDEX)
            split_names,rows=deployment_rows(index)
            package_layers=[]
            for layer_no,split in enumerate(split_names):
                carrier=package/f"split_{split}.apk"
                if not carrier.is_file():raise RuntimeError(f"missing captured split {carrier.name} in {package}")
                with zipfile.ZipFile(carrier) as split_apk:image=split_apk.read(PACK)
                layer=scratch/"layers"/f"{package_no}-{layer_no}";layer.mkdir(parents=True,exist_ok=True)
                image_path=scratch/f"{package_no}-{split}.erofs";image_path.write_bytes(image)
                fuse=subprocess.Popen([str(FUSE),"-f",str(image_path),str(layer)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
                deadline=time.monotonic()+10
                while not os.path.ismount(layer) and fuse.poll() is None and time.monotonic()<deadline:time.sleep(.05)
                if not os.path.ismount(layer):
                    fuse.terminate();detail=fuse.communicate(timeout=5)[1]
                    raise RuntimeError(f"erofsfuse failed to mount {image_path}: {detail}")
                mounted.append((fuse,layer));package_layers.append(layer)
                image_path.unlink()
            package_tree=scratch/f"package-{package_no}";package_tree.mkdir(exist_ok=True)
            materialize(rows,package_layers,package_tree)
            for fuse,layer in reversed(mounted):
                subprocess.run(["fusermount3","-u",str(layer)],check=True)
                fuse.wait(timeout=10)
            mounted.clear()
            mount_dir=package_tree
            if branch:
                mount_dir=package_tree/branch;mount_dir.mkdir()
                for child in list(package_tree.iterdir()):
                    if child==mount_dir:continue
                    shutil.move(str(child),mount_dir/child.name)
            args += ["--erofs-mount",target,str(mount_dir)]
            if package_no==1:runtime_layers.append(mount_dir)
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
        for fuse,layer in reversed(mounted):
            subprocess.run(["fusermount3","-u",str(layer)],check=False)
            try:fuse.wait(timeout=5)
            except subprocess.TimeoutExpired:fuse.terminate()
        if not keep:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL EROFS ldconfig host repro: {error}", file=sys.stderr)
        sys.exit(1)
