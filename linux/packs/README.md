# OSTree APK packs (format v1)

## EROFS deployment carriers

### Layered format (MTPKIDX2)

`erofs_layers.py` writes one complete EROFS image per pack and a `layers.index`
file for the already-merged deployment view. There is no index image, overlay,
or whiteout: removed paths are absent from the index. Initial packs use the
same parent-directory grouping, lexical order, and 64 MiB target policy as
`pack.py`; updates put changed/new regular files in one new pack and reuse
unchanged layer images byte-for-byte. Images are built from deterministic tar
streams with LZ4HC 12, the OSTree commit timestamp, and commit uid/gid/mode
metadata (never host ownership).

`layers.index` begins with `MTPKIDX2`, a layer table (split name, ZIP entry,
image size, SHA-256), then path-sorted records. Each record carries path,
OSTree checksum, kind, symlink target, serialized xattrs, layer-table index,
file size, mtime, uid, gid and mode. A file or symlink's layer index identifies
the complete EROFS image containing it; directories use `0xffffffff`. Symlink
targets remain in the index for metadata-only `readlink` and `stat` operations,
while the layer copy lets `O_PATH|O_NOFOLLOW` opens return native symlink
descriptors that support `readlinkat(fd, "")`.
The hosted `reader.c` accepts both this deployment view and the separate
`MTPKIDX1` object-seed index. The compat proxy builds the reader in
MTPKIDX2-only mode: MTPKIDX1 is no longer a runtime mount format.

Host writer usage: `python3 linux/packs/erofs_layers.py --repo REPO --commit
CHECKSUM --out OUT --mkfs /path/to/mkfs.erofs`; add `--old-index` and
`--old-layers` for an update. The old layer directory contains
`pack-<sha256>.erofs` files. EROFS payloads in APK splits must be STORED and
4096-byte aligned. `StagedPacks` writes the same deterministic PAX tar stream
from OSTree entry metadata, reuses unchanged installed layer images, and
rewrites layers whose dead-byte ratio reaches 0.5. `LinuxAppStoreService`
signs one `pack_<sha256>` APK split per image. The merged `layers.index` is in
the base APK. The separate `objects.index` split remains for OSTree metadata
and update seeding; it does not describe the deployed filesystem view.

`tests/erofs-layers-host.py` exercises A→B reuse, additions, deletions,
file-to-symlink replacement and the C reader. Set `FSCK_EROFS` to make it
extract only the images named by each index, reconstruct A and B, and compare
paths, bytes, types, modes, ownership, symlinks and xattrs with OSTree
checkouts. `tests/erofs-layers-java.py` runs the Android Java layer writer
against the same pinned `mkfs.erofs` and checks its A→B output with fsck.

`erofs_pack.py` builds one read-only EROFS image from an already deployed app
or runtime tree. The default is LZ4HC level 12; `--compression lzma` selects
LZMA level 9 when image size matters more than mount/read cost. The image
keeps source ownership, modes, symlinks, modification times and xattrs. The
filesystem assigns inode numbers in its stable source-tree traversal order;
the host test checks this by building the same tree twice. Build with the
newest stable `erofs-utils` available for the target release. The utility is
GPL-2+, not GPL-3; keep it a pack-time tool and publish its source/license
notices when distributing the binary.

Example:

```sh
python3 linux/packs/erofs_pack.py --tree DEPLOYMENT/files \
  --image OUT/deployment.erofs --metadata OUT/erofs-pack.json
```

The metadata records image size, SHA-256, compressor and the
`zip-local-header-v1` offset rule. `StubGenerator.generateErofsPack` creates a
code-free `erofs_<image-sha256>` APK split. It stores the image as
`assets/matonos/deployment.erofs`, uncompressed, with its payload aligned to
4096 bytes, and embeds `assets/matonos/erofs-pack.json` beside it. The JSON is
covered by APK signing. The reader obtains the data offset from the named
entry's ZIP local header (`header offset + 30 + filename length + extra-field
length`), then checks the exact image size and SHA-256 before mounting. It
must not infer an offset by scanning for the EROFS magic bytes.

The single-image `generateErofsPack` helper above remains for compatibility
tests. The install path uses `generateErofsLayerPack`: every complete image is
stored as `assets/matonos/pack.erofs` in its own signed `pack_<sha256>` split,
with the payload uncompressed and 4096-byte aligned. The merged `layers.index`
is embedded in the signed base APK. `PackageInstaller` inherits unchanged
content-addressed layer splits and removes layers no longer referenced by the
new index.

The tool is pinned to erofs-utils v1.9.4 and built as an x86_64 static-musl
PIE; see `prebuilt/README.md`, `build-mkfs-erofs.sh` and `licenses/` for the
source, rebuild, and GPL-2.0-or-later notice. It is packaged as an executable
JNI library and invoked by path; MatonOS does not link it.

Host check (does not require mount privilege):

```sh
python3 linux/packs/tests/erofs-host.py
FSCK_EROFS=/path/to/fsck.erofs python3 linux/packs/tests/android-pipeline-host.py
```

It creates an app/runtime-shaped fixture, runs `fsck.erofs`, compares the
extracted tree's paths, bytes, ownership and modes, checks symlink and xattr
metadata plus stable inode assignment, and builds an aligned ZIP carrier to
verify the local-header offset and SHA-256 rule. If `unshare -rm` and EROFS
loop mounting are permitted, it mounts directly from the APK payload and
benchmarks stat/open against the extracted tree; otherwise it reports the
mount benchmark as skipped. `MKFS_EROFS`, `FSCK_EROFS` and `DUMP_EROFS` can
select explicit erofs-utils binaries. mkfs time and extracted/image sizes are
reported for each fixture. For mount performance, use the same deployment and
cache state on a host that permits loop mounts; the small synthetic fixture
is an integrity check, not a representative size benchmark.

`android-pipeline-host.py` executes the packaged static tool with the same
deterministic options as the Android staging path, runs fsck, and checks that
two builds of the same tree are byte-identical. `linux/stubgen/tests/run-tests.sh`
checks the signed EROFS split, embedded metadata, image hash/size, and ZIP
payload alignment. No QEMU VM is needed for these host checks.

Real Tux Racer and GNOME Calculator deployment size/time measurements remain
open: there are no local Flatpak deployment trees in the host cache, and the
pinned static Flatpak client cannot validate Flathub TLS on this host
(`remote-add` reports curl error 60). The synthetic integrity fixture result
must not be treated as either application's measurement.

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
its own allocator. Each mounted deployment owns one parsed, immutable index
for the lifetime of that mount; the index bytes travel with that deployment's
signed APK split, and a package update creates a new mount/index instance.
There is no mutable cross-deployment path cache to invalidate or reuse across
updates. Since the writer sorts records by path, path/stat lookups use binary
search and directory enumeration starts at the matching sorted range. Thus
backing offsets, stat/statx metadata and directory entries all come from the
same deployment index. `MTP_FREESTANDING` omits only the libc file-opening
wrapper and heap callbacks; hosted reader users keep the existing filename API.
`../compat-proxy/tests/pack-host.py` exercises real index/ZIP mounts without
extracting the payloads, including execution and dynamic shared-library mmap.

## Java writer and APK integration

`linux/stubgen/src/org/matonos/linuxhost/stubgen/ObjectPacks.java` is a
Java-only writer for the same index, metadata and deterministic content ZIP
format. `initial` and `update` consume canonical records with readable source
files; `update` takes a dead-byte threshold (use `0.5` for the default policy).
Sources for unchanged objects must remain available for repacking. The current
writer rejects ZIP64-sized packs explicitly. `OstreePacks` supplies records from an on-device OSTree repo.
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

At stub launch, `ErofsLaunch` resolves every indexed split through
PackageManager's parallel `splitNames`/`splitSourceDirs` arrays, pins the stub
certificate before the first mount, and asks `matonos-erofsd` to mount each
image with a process-lifetime Binder token. The stub writes the signed base
index and a split-to-mount-path table into its cache, then passes those to the
proxy. MTPKIDX2 metadata serves stat, readdir, readlink and misses; file opens
go directly to the selected EROFS mount and return ordinary kernel fds. The
proxy does not open ZIP carriers or support the former MTPKIDX1 launch path.
The object-seed `PackLaunch` helpers remain for update staging only.

Task 4b adds `OstreePacks` and `StagedPacks`. The reader resolves a revision
with stock `ostree rev-parse`, walks checksum-verified binary commit/dirtree/
dirmeta GVariants, and reads payloads directly from repository object files.
It supports bare-user and the real staging repo's bare-user-only format
(including the latter's real symlink objects). File ownership, mode and xattrs
come from per-path `ostree ls --dironly --checksum --xattrs`; only its fixed
header and restricted GLib byte-array/bytestring grammar are parsed. Paths
come from binary dirtrees, so spaces, newlines, quotes and Unicode do not
require interpreting the CLI's unescaped path/target suffix. Directory xattrs
remain in raw dirmeta objects, as in pack.py. This uses one CLI process per
file; batching is a future performance improvement. No new APEX helper exists.

On device, local CLI calls run the same packaged v3 loader as phase-C installs,
with `--parent-pid`, then the subsystem APK's
`libmatonos-flatpak.so ostree` executable from its native-library directory.
The explicit selector is important: multicall.c shifts argv so ostree receives
its expected argv[0]. Local commands need no DNS transport.

After phase-C Flatpak completes (the installer no longer applies an app's
extra-data; the raw, checksum-verified downloads stay in **extra/** and are
applied by the app's stub), the store reads the real installation repo's app
commit, then commits the deployment's **files/** root, including
**extra/**, to a new local bare-user repo at
`<subsystem-work>/flatpak-staging/<operation>/pack-repo`. This separate staging repository
is intentional: the real download repo is bare-user-only; changing its mode
would invalidate Flatpak's assumptions. The private ref is
`matonos/private/deployed-files`, scoped by the operation directory, with no
parent or remote. It never gets published or uploaded. The local commit is
packed, so index paths are relative to /app (there is no /files prefix).
Upstream app/runtime pins and the verification manifest keep their original
signed identities. The private repo is removed by normal staging cleanup.

The subsystem runs OSTree fsck before producing packs. Flatpak's pull uses the
image's pinned Flathub remote/key. Staging is app-private; no ownership handoff,
sealing daemon or published checkout exists. `deployment.bin` in each signed
index split carries ref, original upstream commit object, origin, metadata and
deploy GVariant. Its bytes participate in the index split's hash. The stub
rebuilds only Flatpak's lookup scaffold in its own cache and mounts payloads
from installed APKs. See [retirement inventory](../../docs/RETIRE-LINUXD.md).


Run `python3 linux/packs/tests/java-source.py` for the actual stock-CLI reader
against host bare-user and bare-user-only repositories, byte equality with
pack.py, framing widths through four bytes, binary/text xattrs, unusual names,
and a final files/extra commit fixture. This does not run apply_extra itself.

## PackageInstaller sessions

Packs are the store's app install format. The base contains only the stub and
its UI/permission metadata. Exactly one `index_<sha256>` code-free split holds
`objects.index` and `objects.ostree`; its name hashes length-prefixed bytes of
both files. Content carriers retain `pack_<sha256>` names. Android split names
use underscores; the mount's ZIP aliases remain `pack-<sha256>.zip`. No C-reader
format change is needed: PackLaunch extracts the index and links only actual
PackageManager-supplied APK paths for the existing proxy `--pack-mount` API.
Missing or multiple index splits fail closed. There is no base-index fallback.

VersionCode 18 identifies this stub template/format, equal in base and every
split. Increase that constant only for a template/format change, using a full
session with every split; never bump it for a content update. Changes to base
contents (including the appstream launcher label/permissions) also require a full session.
Base comparison ignores ZIP timestamps and signature entries, comparing all
actual entry bytes. Every newly generated content/index split receives one plus
the maximum installed base/split revisionCode. Unchanged inherited splits keep
their old revisions; overflow fails explicitly. The base revision stays zero.
A byte-identical installed index/carrier set needs no PackageInstaller session.

Ordinary updates use MODE_INHERIT_EXISTING, write the new index and new/repacked
carriers, inherit unchanged carriers and remove dead splits. The production
packing policy is ObjectPacks.update(..., 0.5), reading the installed index and
using installed APKs as old carriers (indexed payload offsets survive signing).
Only newly generated raw ZIPs go through generatePack; retained signed APKs are
inherited, or copied directly for a full same-version base replacement.

Updates restart the app, as Android app updates do: every content update
replaces the index split, and PackageManager kills the app when an existing split
is replaced or removed. No DONT_KILL_APP is requested.

The adapter streams/fsyncs each APK, removes obsolete names and commits once.
A private asynchronous status broadcast completes the store operation. A pending
user-action result fails/abandons rather than hanging a privileged store request.
Cancellation is accepted before commit; after commit the operation waits for its
actual result, since commit is the point of no return. Session failures abandon
without a second installation attempt. Local APKs, packs and installed-carrier
cache links are removed on completion/failure/cancellation, without following
symlinks. Process death recovery of sessions/cache is still a device lifecycle
check; the current operation registry is in memory.

There is no linuxd publication or registration step. Successful PackageInstaller
commit is the installed state; PackageManager owns updates, uninstall and APK
lifetimes. Runtime/extension code is carried by runtime packages. The stub
rebuilds disposable Flatpak lookup metadata from the signed index split; no
shared deployment lease is acquired. Extra-data still executes in the target
package and its result is a content split.

Final stubs declare their launcher activity enabled in the manifest. Missing
runtime packages trigger the existing authenticated subsystem repair request.
Pre-retirement stubs without deployment.bin must be reinstalled/updated once.

Tests: stubgen/tests/run-tests.sh, stubgen/tests/deployment-flatpak-test.py,
packs/tests/java-writer.py, java-source.py and run.sh. Build the subsystem with
MATON_APPS_ONLY=matonos-linux-subsystem tools/build-apps.sh. No ILinuxd AIDL,
bridge registration or deployment-reconciliation service remains.

### v3.5 updates from installed carriers

The common subsystem `InstallTransaction.Scratch` pull path seeds its private
bare-user-only repository from PackageManager-resolved installed pack slices.
`PackSeed` copies each unique canonical file object once and recreates symlinks;
no deployed filesystem or writable persistent OSTree repository is retained.
Only uid/gid 0, empty-xattr, canonical-mode objects qualify. Other objects are
fetched normally. Pack production uses stock OSTree canonical owner/permission
options so the indexed identities can be reused in the next download.

Pulls use Flatpak's `--no-static-deltas`: ordinary per-object fetching skips
seeded checksums. The packs represent deployed `files/`, not the complete
original Flatpak commit, so claiming a complete static-delta base would be
incorrect. In particular a delta from an empty base could download everything.
Metadata and missing payload objects still come from the authenticated remote;
the existing fsck runs before packaging. `ObjectPacks.update(..., 0.5)` continues
to reuse installed carrier splits.

Before seeding, the subsystem checks available space against the unique base
payload rounded to the actual filesystem block size. This is a payload budget,
not a total-update cap: directory metadata, new downloads, extra-data, the local
pack repo, new packs, signed carriers and PackageInstaller sessions also consume
space. Stock libostree enforces its free-space reserve during downloads; write
failures abort before package replacement. Progress reports base budget/free
bytes and actual allocated operation bytes after each pull; logcat records
allocated operation bytes after packing. Allocated-byte accounting deduplicates
hardlinks by device/inode. Operation cleanup releases the temporary base too.

`linux/stubgen/tests/run-tests.sh` includes a real HTTP OSTree test: commit A is
packed, a fresh repo is seeded only from those packs, and commit B changes one
small file while retaining a random 1 MiB file and a symlink. It asserts one
content-object request, checks fsck, updates packs, builds carriers and checks
payload slices through the production `PackLaunch` path. No image/VM is needed.

### v3.5 static delta update bases

The signed index split additionally carries `original.ostree` (a ZIP of every
original dirtree/dirmeta object) and `original.index` (original file identities
and carrier offsets). `deployment.bin` already carries the original commit.
The deployed mount index remains independent. Packs include original files
outside `files/`, such as Flatpak metadata and exports, as well as deployed
payloads; identical checksums share one slice. Canonical deployed commits retain
original regular-file and symlink identities on apply_extra-free trees. Raw
extra downloads or other deployment changes do not stand in for original objects.

Before updating, PackSeed restores original file objects from installed APK
carriers, restores metadata, rejects partial bases and runs stock OSTree fsck.
Only a complete base enables normal static-delta selection. Missing metadata,
missing objects or failed verification log a reason and retain the existing
per-object pull. Old packages need no migration. Original-only carriers are
retained by both package installation sessions.

`tests/run.sh OUTPUT` includes a real HTTP A→B delta generated by OSTree. The
fixture changes one byte in an incompressible 1 MiB file, verifies fetched delta
bytes against on-disk delta size, requires zero loose object downloads, checks B
and its updated APK slices, and exercises missing/partial-base rejection and
per-object fallback. `OriginalMetadataSizeTest REPO REF OUTPUT` optionally
measures added index assets on a locally available real app without modifying
its source repo. ZIP growth excludes APK signing/alignment overhead.
