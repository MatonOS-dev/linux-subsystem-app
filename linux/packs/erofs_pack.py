#!/usr/bin/env python3
"""Build one immutable EROFS image for a deployed app/runtime tree."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", required=True, type=pathlib.Path)
    parser.add_argument("--image", required=True, type=pathlib.Path)
    parser.add_argument("--metadata", required=True, type=pathlib.Path)
    parser.add_argument("--mkfs", default="mkfs.erofs")
    parser.add_argument("--compression", choices=("lz4hc", "lzma"), default="lz4hc")
    args = parser.parse_args()
    tree = args.tree.resolve(strict=True)
    if not tree.is_dir() or tree.is_symlink():
        parser.error("--tree must be a real directory")
    args.image.parent.mkdir(parents=True, exist_ok=True)
    args.metadata.parent.mkdir(parents=True, exist_ok=True)
    level = "9"
    args.image.unlink(missing_ok=True)
    started = time.perf_counter()
    subprocess.run([args.mkfs, "-z" + args.compression + "," + level, "-Ededupe",
                    "--workers=" + str(max(1, os.cpu_count() or 1)),
                    "--preserve-mtime", str(args.image), str(tree)], check=True)
    elapsed_ms = round((time.perf_counter() - started) * 1000, 3)
    size = args.image.stat().st_size
    metadata = {
        "format": "matonos-erofs-pack-v1",
        "image_entry": "assets/matonos/deployment.erofs",
        "image_size": size,
        "image_sha256": sha256(args.image),
        "offset_rule": "zip-local-header-v1",
        "offset_entry": "assets/matonos/deployment.erofs",
        "compression": args.compression,
        "mkfs_elapsed_ms": elapsed_ms,
    }
    args.metadata.write_text(json.dumps(metadata, sort_keys=True, separators=(",", ":")) + "\n")
    print(json.dumps(metadata, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
