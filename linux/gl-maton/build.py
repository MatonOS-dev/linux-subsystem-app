#!/usr/bin/env python3
"""Build the branch-specific Flatpak GL extension and its signed-pack payload."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.PIPE, text=True)


def build(out, vulkan, egl, branch):
    out = pathlib.Path(out).resolve()
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    files = out / "tree"
    (files / "lib").mkdir(parents=True)
    (files / "share/glvnd/egl_vendor.d").mkdir(parents=True)
    (files / "lib/vulkan/icd.d").mkdir(parents=True)
    shutil.copy2(vulkan, files / "lib/libmaton_vulkan.so")
    shutil.copy2(egl, files / "lib/libEGL_maton.so")
    (files / "lib/vulkan/icd.d/maton_icd.json").write_text(
        '{\n  "file_format_version": "1.0.0",\n  "ICD": {\n'
        '    "library_path": "/usr/lib/x86_64-linux-gnu/GL/maton/lib/libmaton_vulkan.so",\n'
        '    "api_version": "1.4.0"\n  }\n}\n', encoding="utf-8")
    (files / "share/glvnd/egl_vendor.d/10_maton.json").write_text(
        '{\n  "file_format_version": "1.0.0",\n  "ICD": {\n'
        '    "library_path": "libEGL_maton.so"\n  }\n}\n', encoding="utf-8")

    metadata = (ROOT / "linux/gpu-forward-spike/flatpak/metadata.in").read_text()
    metadata = metadata.replace("@RUNTIME_BRANCH@", branch)
    (out / "metadata").write_text(metadata, encoding="utf-8")
    repo = out / "repo"
    run("ostree", "init", "--repo", str(repo), "--mode=bare-user-only")
    ref = f"runtime/org.freedesktop.Platform.GL.maton/x86_64/{branch}"
    run("ostree", "commit", "--repo", str(repo), "--branch", ref,
        "--tree=dir=" + str(files), "--no-xattrs", "--owner-uid=0", "--owner-gid=0",
        "--canonical-permissions")
    commit = subprocess.check_output(["ostree", "rev-parse", "--repo", str(repo), ref], text=True).strip()
    packs = out / "payload"
    subprocess.run(["python3", str(ROOT / "linux/packs/pack.py"), "pack", "--repo", str(repo),
                    "--commit", commit, "--out", str(packs)], check=True)
    (out / "ref.txt").write_text(ref + "\n" + commit + "\n", encoding="utf-8")
    (out / "branch.txt").write_text(branch + "\n", encoding="utf-8")
    shutil.copy2(packs / "objects.index", out / "objects.index")
    shutil.copy2(packs / "objects.ostree", out / "objects.ostree")
    for packed in packs.glob("pack-*.zip"):
        shutil.copy2(packed, out / packed.name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("vulkan")
    parser.add_argument("egl")
    parser.add_argument("--branch", default="26.08")
    args = parser.parse_args()
    if not args.branch or "/" in args.branch:
        parser.error("invalid runtime extension branch")
    build(args.out, args.vulkan, args.egl, args.branch)
