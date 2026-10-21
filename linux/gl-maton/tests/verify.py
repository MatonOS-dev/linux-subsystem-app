#!/usr/bin/env python3
import pathlib
import sys
import zipfile
import gi
from gi.repository import GLib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "packs"))
import pack

root = pathlib.Path(sys.argv[1])
metadata = (root / "metadata").read_text()
assert "name=org.freedesktop.Platform.GL.maton" in metadata
assert "runtime=org.freedesktop.Platform/x86_64/26.08" in metadata
assert "ref=runtime/org.freedesktop.Platform/x86_64/26.08" in metadata
assert (root / "ref.txt").read_text().splitlines()[0] == \
       "runtime/org.freedesktop.Platform.GL.maton/x86_64/26.08"
ref, commit = (root / "ref.txt").read_text().splitlines()
deployment = GLib.Variant.new_from_bytes(
    GLib.VariantType.new("(ssasta{sv})"),
    GLib.Bytes.new((root / "deploy").read_bytes()), True)
assert deployment.get_child_value(0).unpack() == "flathub"
assert deployment.get_child_value(1).unpack() == commit
deploy_metadata = deployment.get_child_value(4)
assert deploy_metadata.lookup_value("deploy-version", None).unpack() == 4
assert deploy_metadata.lookup_value("extension-of", None).unpack() == \
       "runtime/org.freedesktop.Platform/x86_64/26.08"
rows = pack.read_index(root / "objects.index")
paths = {row["path"]: row for row in rows}
expected = {
    "/lib/libmaton_vulkan.so",
    "/lib/libEGL_maton.so",
    "/lib/libGLX_maton.so.0",
    "/vulkan/icd.d/maton_icd.json",
    "/glvnd/egl_vendor.d/10_maton.json",
}
assert expected <= paths.keys(), paths.keys()
assert "/lib/vulkan/icd.d/maton_icd.json" not in paths  # merge-dirs only sees the extension root
pack_files = {path.name for path in root.glob("pack-*.zip")}
assert {row["pack"] for row in rows if row["kind"] == "file"} == pack_files

def payload(path):
    row = paths[path]
    with zipfile.ZipFile(root / row["pack"]) as archive:
        return archive.read(row["checksum"]).decode()

assert '"library_path": "libmaton_vulkan.so"' in payload(
    "/vulkan/icd.d/maton_icd.json")
assert '"library_path": "libEGL_maton.so"' in payload("/glvnd/egl_vendor.d/10_maton.json")
assert (root / "objects.ostree").read_bytes().startswith(b"MTPKOBJ1")
