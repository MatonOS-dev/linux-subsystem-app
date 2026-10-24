#!/usr/bin/env python3
"""Build the branch-specific Flatpak GL extension and its signed-pack payload."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile

import gi
from gi.repository import GLib

ROOT = pathlib.Path(__file__).resolve().parents[2]


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.PIPE, text=True)


def build(out, vulkan, egl, glx, branch, vulkan_i386, egl_i386, glx_i386):
    out = pathlib.Path(out).resolve()
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    files = out / "tree"
    (files / "lib").mkdir(parents=True)
    (files / "glvnd/egl_vendor.d").mkdir(parents=True)
    (files / "vulkan/icd.d").mkdir(parents=True)
    shutil.copy2(vulkan, files / "lib/libmaton_vulkan.so")
    if vulkan_i386:
        shutil.copy2(vulkan_i386, files / "lib/libmaton_vulkan_i386.so")
    shutil.copy2(egl, files / "lib/libEGL_maton.so")
    shutil.copy2(glx, files / "lib/libGLX_maton.so.0")
    (files / "lib/libGLX_maton.so").write_bytes((files / "lib/libGLX_maton.so.0").read_bytes())
    if egl_i386 or glx_i386:
        i386_lib = files / "lib/i386-linux-gnu"
        i386_lib.mkdir(parents=True)
        if egl_i386:
            shutil.copy2(egl_i386, i386_lib / "libEGL_maton.so")
        if glx_i386:
            shutil.copy2(glx_i386, i386_lib / "libGLX_maton.so.0")
            (i386_lib / "libGLX_maton.so").write_bytes((i386_lib / "libGLX_maton.so.0").read_bytes())
    (files / "vulkan/icd.d/maton_icd.json").write_text(
        '{\n  "file_format_version": "1.0.0",\n  "ICD": {\n'
        '    "library_path": "libmaton_vulkan.so",\n'
        '    "api_version": "1.4.0"\n  }\n}\n', encoding="utf-8")
    if vulkan_i386:
        (files / "vulkan/icd.d/maton_icd_i386.json").write_text(
            '{\n  "file_format_version": "1.0.0",\n  "ICD": {\n'
            '    "library_path": "libmaton_vulkan_i386.so",\n'
            '    "api_version": "1.4.0"\n  }\n}\n', encoding="utf-8")
    (files / "glvnd/egl_vendor.d/10_maton.json").write_text(
        '{\n  "file_format_version": "1.0.0",\n  "ICD": {\n'
        '    "library_path": "libEGL_maton.so"\n  }\n}\n', encoding="utf-8")
    if egl_i386:
        (files / "glvnd/egl_vendor.d/10_maton_i386.json").write_text(
            '{\n  "file_format_version": "1.0.0",\n  "ICD": {\n'
            '    "library_path": "i386-linux-gnu/libEGL_maton.so"\n  }\n}\n', encoding="utf-8")

    metadata = (ROOT / "linux/gpu-bridge-common/flatpak/metadata.in").read_text()
    metadata = metadata.replace("@RUNTIME_BRANCH@", branch)
    (out / "metadata").write_text(metadata, encoding="utf-8")
    repo = out / "repo"
    run("ostree", "init", "--repo", str(repo), "--mode=bare-user-only")
    ref = f"runtime/org.freedesktop.Platform.GL.maton/x86_64/{branch}"
    run("ostree", "commit", "--repo", str(repo), "--branch", ref,
        "--tree=dir=" + str(files), "--no-xattrs", "--owner-uid=0", "--owner-gid=0",
        "--canonical-permissions", "--no-bindings")
    commit = subprocess.check_output(["ostree", "rev-parse", "--repo", str(repo), ref], text=True).strip()
    runtime_ref = f"runtime/org.freedesktop.Platform/x86_64/{branch}"
    deploy_metadata = GLib.Variant.parse(
        GLib.VariantType.new("a{sv}"),
        "{'deploy-version': <int32 4>, 'timestamp': <uint64 0>, "
        f"'runtime': <'org.freedesktop.Platform/x86_64/{branch}'>, "
        f"'extension-of': <'{runtime_ref}'>}}",
        None)
    deploy = GLib.Variant.parse(
        GLib.VariantType.new("(ssasta{sv})"),
        f"('flathub', '{commit}', [], uint64 0, {deploy_metadata.print_(True)})",
        None)
    (out / "deploy").write_bytes(deploy.get_data_as_bytes().get_data())
    packs = out / "payload"
    subprocess.run(["python3", str(ROOT / "linux/packs/pack.py"), "pack", "--repo", str(repo),
                    "--commit", commit, "--out", str(packs)], check=True)
    (out / "ref.txt").write_text(ref + "\n" + commit + "\n", encoding="utf-8")
    shutil.copy2(packs / "objects.index", out / "objects.index")
    shutil.copy2(packs / "objects.ostree", out / "objects.ostree")
    for packed in packs.glob("pack-*.zip"):
        shutil.copy2(packed, out / packed.name)

    # The i386 runtime has its own Flatpak GL extension point. Reusing the
    # x86_64 ref would expose both ELF classes to the 32-bit loader and makes
    # Flatpak mount the tree at the wrong multiarch directory.
    if vulkan_i386 and egl_i386 and glx_i386:
        build_i386(out / "gl32", vulkan_i386, egl_i386, glx_i386, branch)


def build_i386(out, vulkan, egl, glx, branch):
    out = pathlib.Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    files = out / "tree"
    (files / "lib").mkdir(parents=True)
    (files / "glvnd/egl_vendor.d").mkdir(parents=True)
    (files / "vulkan/icd.d").mkdir(parents=True)
    # GL32 is mounted at the i386-specific .../GL/maton extension directory,
    # so its library directory is just lib/ (the multiarch path is already
    # supplied by the mount point).
    shutil.copy2(egl, files / "lib/libEGL_maton.so")
    shutil.copy2(glx, files / "lib/libGLX_maton.so.0")
    (files / "lib/libGLX_maton.so").write_bytes(
        (files / "lib/libGLX_maton.so.0").read_bytes())
    shutil.copy2(vulkan, files / "lib/libmaton_vulkan_i386.so")
    (files / "vulkan/icd.d/maton_icd_i386.json").write_text(
        '{\n  "file_format_version": "1.0.0",\n  "ICD": {\n'
        '    "library_path": "libmaton_vulkan_i386.so",\n'
        '    "api_version": "1.4.0"\n  }\n}\n', encoding="utf-8")
    (files / "glvnd/egl_vendor.d/10_maton_i386.json").write_text(
        '{\n  "file_format_version": "1.0.0",\n  "ICD": {\n'
        '    "library_path": "libEGL_maton.so"\n  }\n}\n', encoding="utf-8")
    (out / "metadata").write_text(
        f"[Runtime]\nname=org.freedesktop.Platform.GL32.maton\nruntime=org.freedesktop.Platform/x86_64/{branch}"
        f"\nsdk=org.freedesktop.Sdk/x86_64/{branch}\n\n[ExtensionOf]\nref=runtime/org.freedesktop.Platform/x86_64/{branch}\n",
        encoding="utf-8")
    repo = out / "repo"
    run("ostree", "init", "--repo", str(repo), "--mode=bare-user-only")
    ref = f"runtime/org.freedesktop.Platform.GL32.maton/x86_64/{branch}"
    run("ostree", "commit", "--repo", str(repo), "--branch", ref,
        "--tree=dir=" + str(files), "--no-xattrs", "--owner-uid=0", "--owner-gid=0",
        "--canonical-permissions", "--no-bindings")
    commit = subprocess.check_output(["ostree", "rev-parse", "--repo", str(repo), ref], text=True).strip()
    runtime_ref = f"runtime/org.freedesktop.Platform/x86_64/{branch}"
    deploy_metadata = GLib.Variant.parse(
        GLib.VariantType.new("a{sv}"),
        "{'deploy-version': <int32 4>, 'timestamp': <uint64 0>, "
        f"'runtime': <'org.freedesktop.Platform/x86_64/{branch}'>, "
        f"'extension-of': <'{runtime_ref}'>}}", None)
    deploy = GLib.Variant.parse(
        GLib.VariantType.new("(ssasta{sv})"),
        f"('flathub', '{commit}', [], uint64 0, {deploy_metadata.print_(True)})", None)
    (out / "deploy").write_bytes(deploy.get_data_as_bytes().get_data())
    packs = out / "payload"
    subprocess.run(["python3", str(ROOT / "linux/packs/pack.py"), "pack", "--repo", str(repo),
                    "--commit", commit, "--out", str(packs)], check=True)
    (out / "ref.txt").write_text(ref + "\n" + commit + "\n", encoding="utf-8")
    shutil.copy2(packs / "objects.index", out / "objects.index")
    shutil.copy2(packs / "objects.ostree", out / "objects.ostree")
    for packed in packs.glob("pack-*.zip"):
        shutil.copy2(packed, out / packed.name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("vulkan")
    parser.add_argument("egl")
    parser.add_argument("--glx", required=True, help="built libGLX_maton.so")
    parser.add_argument("--vulkan-i386", help="freestanding ELF32 Vulkan forwarder")
    parser.add_argument("--egl-i386", help="freestanding ELF32 EGL forwarder")
    parser.add_argument("--glx-i386", help="freestanding ELF32 GLX vendor")
    parser.add_argument("--branch", default="26.08")
    args = parser.parse_args()
    if not args.branch or "/" in args.branch:
        parser.error("invalid runtime extension branch")
    build(args.out, args.vulkan, args.egl, args.glx, args.branch,
          args.vulkan_i386, args.egl_i386, args.glx_i386)
