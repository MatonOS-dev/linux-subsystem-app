#!/usr/bin/env python3
"""Store host JNI/dex before zipalign and signing; keep extraction requested."""
import copy
import re
import sys
import zipfile


def prepare(source, destination):
    with zipfile.ZipFile(source) as src, zipfile.ZipFile(destination, "w") as dst:
        names = src.namelist()
        if len(names) != len(set(names)):
            raise ValueError("Duplicate APK entries")
        for required in ("classes.dex", "lib/x86_64/libmaton_compositor.so",
                         "lib/x86_64/libmatonos-dbus-broker.so"):
            if required not in names:
                raise ValueError(f"Missing host APK entry: {required}")
        for entry in src.infolist():
            # Drop v1 signatures; rewriting also drops the ZIP's v2/v3 block.
            if re.fullmatch(r"META-INF/(MANIFEST\.MF|[^/]+\.(SF|RSA|DSA|EC))",
                            entry.filename, re.IGNORECASE):
                continue
            data = src.read(entry.filename)
            entry = copy.copy(entry)
            if entry.filename.endswith(".dex") or (
                    entry.filename.startswith("lib/") and entry.filename.endswith(".so")):
                entry.compress_type = zipfile.ZIP_STORED
            dst.writestr(entry, data)


if __name__ == "__main__":
    prepare(*sys.argv[1:])
