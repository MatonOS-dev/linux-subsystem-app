#!/usr/bin/env python3
"""Materialize the bundled Maton GL extension like PackageDeployment."""
import io
import pathlib
import re
import sys
import zipfile


def read_text(archive, name):
    return archive.read(name).decode("utf-8")


def materialize(archive, system, prefix, extension, branch):
    identity = read_text(archive, f"{prefix}/ref.txt").strip().splitlines()
    ref_prefix = f"runtime/org.freedesktop.Platform.{extension}/x86_64/"
    if len(identity) != 2 or not identity[0].startswith(ref_prefix) or not re.fullmatch(r"[0-9a-f]{64}", identity[1]):
        raise ValueError(f"invalid bundled Flatpak extension identity: {prefix}")
    commit = identity[1]
    commit_path = f"{prefix}/repo/objects/{commit[:2]}/{commit[2:]}.commit"
    commit_object = archive.read(commit_path)
    ref = ref_prefix + branch
    version = system / ref / commit
    files = version / "files"
    files.mkdir(parents=True, exist_ok=True)
    if extension == "GL.maton":
        metadata = (
            "[Runtime]\nname=org.freedesktop.Platform.GL.maton\n"
            f"runtime=org.freedesktop.Platform/x86_64/{branch}\n"
            f"sdk=org.freedesktop.Sdk/x86_64/{branch}\n\n"
            "[ExtensionOf]\nref=runtime/org.freedesktop.Platform/x86_64/"
            f"{branch}\n"
        )
    else:
        metadata = (
            "[Runtime]\nname=org.freedesktop.Platform.GL32.maton\n"
            f"runtime=org.freedesktop.Platform/x86_64/{branch}\n"
            f"sdk=org.freedesktop.Sdk/x86_64/{branch}\n\n"
            "[ExtensionOf]\nref=runtime/org.freedesktop.Platform/x86_64/"
            f"{branch}\n"
        )
    (version / "metadata").write_text(metadata)
    (version / "deploy").write_bytes(archive.read(f"{prefix}/deploy"))
    (version / ".ref").touch()
    tree_prefix = f"{prefix}/tree/"
    for item in archive.infolist():
        if not item.filename.startswith(tree_prefix):
            continue
        relative = pathlib.PurePosixPath(item.filename[len(tree_prefix):])
        if not relative.parts:
            continue
        destination = files.joinpath(*relative.parts)
        if not destination.resolve().is_relative_to(files.resolve()):
            raise ValueError(f"invalid extension path: {item.filename}")
        if item.is_dir():
            destination.mkdir(parents=True, exist_ok=True)
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(item) as source, destination.open("wb") as output:
                while True:
                    block = source.read(1024 * 1024)
                    if not block:
                        break
                    output.write(block)
    branch_root = system / ref
    active = branch_root / "active"
    if active.is_symlink() or active.exists():
        active.unlink()
    active.symlink_to(pathlib.Path(commit))
    current = system / "runtime" / f"org.freedesktop.Platform.{extension}" / "current"
    current.parent.mkdir(parents=True, exist_ok=True)
    if current.is_symlink() or current.exists():
        current.unlink()
    current.symlink_to(pathlib.Path("x86_64") / branch)
    repo = system / "repo"
    object_path = repo / "objects" / commit[:2] / f"{commit[2:]}.commit"
    object_path.parent.mkdir(parents=True, exist_ok=True)
    object_path.write_bytes(commit_object)
    for directory in (repo / "refs/heads", repo / "refs/remotes", repo / "tmp"):
        directory.mkdir(parents=True, exist_ok=True)
    (repo / "config").write_text("[core]\nrepo_version=1\nmode=bare-user-only\n")
    return files


def main(args):
    if len(args) != 6:
        raise SystemExit("usage: materialize-gl.py SUBSYSTEM_APK SYSTEM_ROOT GL_BRANCH RUNTIME_BRANCH WANT_GL32")
    apk_path, system_path, gl_branch, runtime_branch, want_gl32 = args[1:]
    with zipfile.ZipFile(apk_path) as outer:
        embedded = outer.read("assets/MatonLinuxSubsystem.apk")
    with zipfile.ZipFile(io.BytesIO(embedded)) as archive:
        system = pathlib.Path(system_path)
        gl = materialize(archive, system, "assets/gl-maton", "GL.maton", gl_branch)
        gl32 = ""
        if want_gl32 == "1":
            gl32 = str(materialize(archive, system, "assets/gl-maton-i386", "GL32.maton", runtime_branch))
    print(f"GL\t{gl}")
    if gl32:
        print(f"GL32\t{gl32}")


if __name__ == "__main__":
    main(sys.argv)
