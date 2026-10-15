# OSTree APK packs (format v1)

`pack.py` is a Python 3 host-tool prototype. It uses the `ostree` executable
to check out a commit and emits a base index plus immutable ZIP content packs.
`reader.c` is a small C99 reader for that index. Run `./tests/run.sh` for a
two-commit fixture and update/replay tests.

## Files and records

The base artifact contains `commit.ostree` (the raw `ostree show --raw`
output) and `objects.index` (the binary index described below). The index has
magic `MTPKIDX1`, a little
endian u32 record count, then records sorted by UTF-8 path bytes. Each record
contains u32-length-prefixed path, checksum, pack filename and xattr blob,
followed by little-endian u64 offset, u64 length, u32 uid, u32 gid and u32
mode. The xattr blob is the `user.ostreemeta` value read from the checkout
(empty when absent).
Offsets point to file data, not ZIP headers; they are 4096-byte aligned.

Each pack is an ordinary ZIP with STORED entries named by OSTree object
checksum. ZIP local headers carry a private extra field whose padding places
the first data byte on a 4096-byte boundary. Central directory records are
ordinary ZIP records. Pack names are `pack-<sha256-of-complete-zip>.zip`.
The archive is deterministic for a given object set and pack policy.

The index records paths as well as checksums: this preserves per-path mode,
ownership and xattrs for hardlinked content objects. Current implementation
records regular files only.

## CLI

`pack.py pack --repo REPO --commit CHECKSUM --out DIR [--pack-size BYTES]`
and `pack.py update --repo REPO --commit CHECKSUM --old-index FILE
--old-packs DIR --out DIR [--dead-ratio 0.5] [--pack-size BYTES]`.
The repository should be in OSTree's `bare-user` mode. First install groups
files by parent directory and fills target-sized packs in lexical order.
Updates retain old pack references for still-live objects, create one new
pack from missing objects, and rewrite touched packs only when dead bytes /
total bytes reaches the requested threshold. Use `--dead-ratio 1` to avoid
threshold-driven rewriting; `--dead-ratio 0` rewrites every touched pack.
Output includes bytes written and live bytes changed (new or copied live
payload bytes), plus their amplification ratio.

Current prototype limitation: checked-out regular-file bytes are addressed
by SHA-256 of their payload. The tool does not yet resolve OSTree's canonical
file-object checksums or include dirtree/dirmeta object bytes, and it omits
directories, symlinks and other non-regular entries. A production APK
producer must replace the checkout scanner with a bare-user object reader
before consuming this format. The test fixture supplies the checkout commands
because `ostree` is not installed on the implementation host. The tool does
not mutate a repository.
