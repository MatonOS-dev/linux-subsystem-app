#!/usr/bin/env python3
"""Regression test for Flathub's multiarch Compat.i386 metadata."""
import pathlib
import struct
import subprocess
import sys
import tempfile

helper = pathlib.Path(sys.argv[1])


def response(metadata, ref):
    with tempfile.NamedTemporaryFile("w", encoding="utf-8") as file:
        file.write(metadata)
        file.flush()
        data = subprocess.check_output([str(helper), "--requirements", file.name, ref])
    offset = 0

    def string():
        nonlocal offset
        size = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        value = data[offset:offset + size].decode()
        offset += size
        return value

    runtime, parent = string(), string()
    has_extensions = bool(struct.unpack_from(">I", data, offset)[0])
    offset += 4
    assert offset == len(data)
    return runtime, parent, has_extensions


compat = "runtime/org.freedesktop.Platform.Compat.i386/x86_64/26.08"
compat_metadata = """[Runtime]
name=org.freedesktop.Platform.Compat.i386

[ExtensionOf]
ref=runtime/org.freedesktop.Platform/i386/26.08
"""
assert response(compat_metadata, compat) == ("", "", False)

gl32 = "runtime/org.freedesktop.Platform.GL32.default/x86_64/26.08"
gl32_metadata = """[Runtime]
name=org.freedesktop.Platform.GL.default
runtime=org.freedesktop.Platform/i386/26.08

[ExtensionOf]
ref=runtime/org.freedesktop.Platform/i386/26.08
"""
assert response(gl32_metadata, gl32) == ("", "", False)

locale = "runtime/org.freedesktop.Platform.Locale/x86_64/26.08"
locale_metadata = """[Runtime]
name=org.freedesktop.Platform.Locale

[ExtensionOf]
ref=runtime/org.freedesktop.Platform/x86_64/26.08
"""
assert response(locale_metadata, locale) == (
    "", "runtime/org.freedesktop.Platform/x86_64/26.08", False
)

steam = "app/com.valvesoftware.Steam/x86_64/stable"
steam_metadata = """[Application]
name=com.valvesoftware.Steam
runtime=org.freedesktop.Platform/x86_64/26.08

[Extension org.freedesktop.Platform.Compat.i386]
directory=lib/i386-linux-gnu
version=26.08
add-ld-path=lib
allow=multiarch
"""
assert response(steam_metadata, steam) == (
    "org.freedesktop.Platform/x86_64/26.08", "", True
)
print("PASS Steam multiarch and GL32 metadata retain x86_64 refs and do not plan the nonexistent i386 parent")
