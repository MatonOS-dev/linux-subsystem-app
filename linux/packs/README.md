# OSTree APK packs (format v1)

`pack.py` is a Python 3 host tool using PyGObject/libostree 2024.5. It reads
real OSTree repositories in `bare-user` mode. `reader.c` is the C99 lookup
library intended for the compat proxy. Run `./tests/run.sh` to generate a
three-commit repo and verify checkout reconstruction and update replay.

## Base artifact

The base consists of `objects.ostree` and `objects.index`. `objects.ostree`
starts with `MTPKOBJ1`, a little endian u32 object count, then records sorted
by canonical checksum: 32 raw checksum bytes, little endian u64 object length,
and the exact raw libostree object bytes. It contains the commit, every
reachable dirtree, and every reachable dirmeta object, unchanged from the
repository. This lets a client walk the tree and verify metadata with
libostree.

`objects.index` starts with `MTPKIDX1`, a little endian u32 entry count, then
records in UTF-8 path order. Each record contains five u32-length-prefixed
strings (`path`, canonical OSTree `checksum`, pack filename, kind, symlink
target), a u32-length-prefixed xattr byte string, then little endian u64
payload offset, u64 payload length, u32 uid, u32 gid and u32 mode. Kinds are
`dir`, `file`, and `symlink`. Directory and symlink records have no pack
payload; their canonical checksums refer to OSTree dirtree/file objects. File
checksums are canonical OSTree file-object checksums, not hashes of payload
bytes. The xattr field is the serialized `a(ayay)` xattr variant returned by
libostree for bare-user files. Directory metadata, including ownership and
mode, is also retained in the verbatim dirmeta objects.

## Content packs

Packs are immutable STORED ZIP files. Each entry is named by its canonical
OSTree file-object checksum and contains the bare-user file content payload
only. Local ZIP headers include a private alignment extra field that places
each payload at a 4096-byte boundary; index offsets point directly to those
bytes, not to ZIP headers. Pack filenames are `pack-<sha256-of-complete-zip>.zip`.
Creation is deterministic for a given object set and pack policy. Initial
packing groups payloads by parent directory, filling target-sized packs in
lexical order; pack size is a policy target, not a cap.

## Updates

`pack.py update --repo REPO --commit CHECKSUM --old-index INDEX --old-packs DIR
--out DIR [--dead-ratio 0.5]` compares canonical checksums. It writes one pack
for file objects absent from the old index, keeps live objects in their
existing packs, and copies referenced immutable packs into the output to make
it self-contained. Old packs whose dead-byte ratio reaches the threshold are
rewritten from their still-live payloads. `--dead-ratio 1` effectively avoids
rewrite; `--dead-ratio 0` rewrites touched packs. Output reports payload bytes
written, changed and their ratio.

## C reader

`reader.h` exposes `mtp_index_open`, path lookup/stat, immediate-child readdir and
entry metadata (kind, mode, uid, gid, checksum, payload location, xattrs and
symlink target). A caller stats an entry from those returned metadata fields;
directories and symlinks have no pack offset. The reader validates truncation
and integer bounds while loading the index.

The compat proxy consumes this reader through `mtp_index_open_buffer` with
its own allocator. `MTP_FREESTANDING` omits only the libc file-opening wrapper
and heap callbacks; hosted reader users keep the existing filename API.
`../compat-proxy/tests/pack-host.py` exercises real index/ZIP mounts without
extracting the payloads, including execution and dynamic shared-library mmap.

## Java writer and APK integration building blocks (task 4a, partial)

`linux/stubgen/src/org/matonos/linuxhost/stubgen/ObjectPacks.java` is a
Java-only writer for the same index, metadata and deterministic content ZIP
format. `initial` and `update` consume canonical records with readable source
files; `update` takes a dead-byte threshold (use `0.5` for the default policy).
Sources for unchanged objects must remain available for repacking. The current
writer rejects ZIP64-sized packs explicitly. It does not read an OSTree repo.
Run `python3 linux/packs/tests/java-writer.py` to compare all output bytes,
including raw metadata, with `pack.py` over three actual bare-user commits and
four update thresholds. That test obtains canonical records through host
libostree; it does not establish an Android-side OSTree reader.

`StubGenerator.generate(..., packDirectory)` embeds `objects.index` and
`objects.ostree` under `assets/matonos/` in the base. `generatePack` appends a
code-free binary manifest to the original ZIP without relocating any payload,
then signs with the subsystem UID's existing AndroidKeyStore stub key. The
split is named `pack_<content-ZIP-sha256>`: the identifier prefix is required
for an Android split name. The index retains `pack-<hash>.zip`. The hash names
the immutable original content ZIP, before manifest insertion and signing;
it is deliberately not the signed APK hash. Base and split version codes
must match. Signing preserves alignment, covered by a real host signing test
and AOSP aapt2 manifest parsing in `linux/stubgen/tests/run-tests.sh`.

`PackLaunch.arguments` resolves those names using PackageManager's parallel
`splitNames`/`splitSourceDirs` arrays. It writes only the base index and tiny
symlinks into the stub cache; the existing `--pack-mount /app INDEX DIRECTORY`
interface then opens and pins real APK carriers. No content is extracted.
V3Launcher adds these arguments when the base contains the pack index; legacy
bases retain their current launch path. The shared runtime setup is unchanged.

This is not an integrated pack installer yet. LinuxAppStoreService does not
call these generator APIs, no on-device canonical OSTree reader is shipped,
and PackageInstaller still uses its existing full-base session. In particular,
`apply_extra` output is not yet packed; a safe extra-tree representation and
its relationship to linuxd's sanitized verification result still need wiring.
App-tree publication remains necessary for existing installs and Flatpak's
installation metadata/dependency lookup, and has not been removed. Do not
interpret host writer/signer/argument tests as a successful device install.
