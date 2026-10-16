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

`PackLaunch.arguments` resolves those names using PackageManager's parallel
`splitNames`/`splitSourceDirs` arrays. It writes only the base index and tiny
symlinks into the stub cache; the existing `--pack-mount /app INDEX DIRECTORY`
interface then opens and pins real APK carriers. No content is extracted.
V3Launcher adds these arguments when the base contains the pack index; legacy
bases retain their current launch path. The shared runtime setup is unchanged.

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
with `--parent-pid`, then `/apex/com.matonos.flatpak/bin/matonos-flatpak ostree`.
The explicit selector is important: multicall.c shifts argv so ostree receives
its expected argv[0]. Local commands need no DNS transport.

After phase-C Flatpak completes (including apply_extra), the store reads the
real installation repo's app commit, then commits the deployment's **files/**
root, including **extra/**, to a new local bare-user repo at
`<staging>/<host-uid>/<operation>/pack-repo`. This separate staging repository
is intentional: the real download repo is bare-user-only; changing its mode
would invalidate linuxd's assumptions. The private ref is
`matonos/private/deployed-files`, scoped by the operation directory, with no
parent or remote. It never gets published or uploaded. The local commit is
packed, so index paths are relative to /app (there is no /files prefix).
Upstream app/runtime pins and the verification manifest keep their original
signed identities. The private repo is removed by normal staging cleanup.

Packing runs before `verify_staged_install` takes staging away. After successful
verification, LinuxAppStoreService invokes StubGenerator to create and log
`base.apk` plus `pack_<hash>.apk` artifacts under a unique
`<host-cache>/flatpak-pack-artifacts/stage-*` directory. Raw index, metadata and
ZIPs live in its `objects/` subdirectory. Artifacts remain in cache for task 4c
inspection. PackageInstaller still installs a separately generated legacy
base only: split sessions, incremental update selection and artifact retention/
cleanup policy are deliberately not wired yet.

This is not an integrated pack installer yet. In particular, linuxd separately
reconstructs and sanitizes its verified published deployment; the packed
phase-C files are not claimed to be byte-equivalent to that sanitized tree.
Before enabling split sessions, review that trust/sanitization boundary and
validate apply_extra and absolute symlink behavior on device.
App-tree publication remains necessary for existing installs and Flatpak's
installation metadata/dependency lookup, and has not been removed. Do not
interpret host writer/signer/argument tests as a successful device install.

Run `python3 linux/packs/tests/java-source.py` for the actual stock-CLI reader
against host bare-user and bare-user-only repositories, byte equality with
pack.py, framing widths through four bytes, binary/text xattrs, unusual names,
and a final files/extra commit fixture. This does not run apply_extra itself.

## Session planning constraints (task 4c)

`PackSessionPlan` is an Android-independent planner with a fake installer seam.
It plans base + all desired content splits for a first install, and a new base
+ missing splits + removed split names for an inherited update. Desired split
names must come from the new index, installed names from PackageManager.
Repacked content receives a different hash/name. An existing content name is
immutable and inherited. The planner is not connected to LinuxAppStoreService.
The host stub test runner covers planning, rejected versions, commit/write/remove
failures and cancellation before and during a session.

Stock Android imposes two constraints that the install policy must resolve:

- `PackageInstallerSession` checks an inherited session's version against the
  existing package even if every split is replaced. Increasing versionCode
  requires MODE_FULL_INSTALL with every carrier regenerated to match the new
  base version. Inheriting unchanged carriers requires a fixed versionCode.
  An increasing base revisionCode is a possible separate update counter; it is
  not an increasing versionCode and is not implemented by this planner.
- Supplying a base clears INSTALL_DONT_KILL_APP in both session modes. Since
  objects.index is in base, pack index updates restart the Android app. The
  planner requests dontKillApp only for an inherited session without a base;
  the current pack format never uses that case for an index change.

Source evidence in this checkout: frameworks/base/services/core/java/com/android/
server/pm/PackageInstallerSession.java, assertPackageConsistentLocked and the
MODE_INHERIT_EXISTING validation branch (Existing version check, base replacement
clears setDontKillApp). Do not patch AOSP to bypass these checks.

### PackageInstaller sessions (task 4c2)

Packs are the store's app install format. The base contains only the stub and
its UI/permission metadata. Exactly one `index_<sha256>` code-free split holds
`objects.index` and `objects.ostree`; its name hashes length-prefixed bytes of
both files. Content carriers retain `pack_<sha256>` names. Android split names
use underscores; the mount's ZIP aliases remain `pack-<sha256>.zip`. No C-reader
format change is needed: PackLaunch extracts the index and links only actual
PackageManager-supplied APK paths for the existing proxy `--pack-mount` API.
Missing or multiple index splits fail closed. There is no base-index fallback.

VersionCode 17 identifies this stub template/format, equal in base and every
split. Increase that constant only for a template/format change, using a full
session with every split; never bump it for a content update. Changes to base
contents (including desktop label/permissions) also require a full session.
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

**Stock limitation: split removal restarts the app.** This checkout's
`frameworks/base/services/core/java/com/android/server/pm/PackageInstallerSession.java`
lines 4813–4814 and 5164–5182 compare package names and versionCode, not
revisionCode. `core/java/android/content/pm/parsing/ApkLiteParseUtils.java`
222–225 requires matching versionCodes; 438 reads per-APK revisionCode.
`PackageManagerServiceUtils.java` 1449–1473 rejects decreasing base or matching
split revisions, not different revisions across APKs. A changed split revision
therefore does not require a changed base revision. However,
PackageInstallerSession 4836–4858 clears DONT_KILL_APP on **any** replacement or
removal of an existing split. Every changed index requires removing its old
split. The adapter requests setDontKillApp(true) only for a base-free inherited
session, but stock PM clears it. Non-restarting index replacement is unavailable
under the zero-framework-patches rule. A device test must confirm this behavior,
not assume the requested flag guarantees process survival.

The adapter streams/fsyncs each APK, removes obsolete names and commits once.
A private asynchronous status broadcast completes the store operation. A pending
user-action result fails/abandons rather than hanging a privileged store request.
Cancellation is accepted before commit; after commit the operation waits for its
actual result, since commit is the point of no return. Session failures abandon
without a second installation attempt. Local APKs, packs and installed-carrier
cache links are removed on completion/failure/cancellation, without following
symlinks. Process death recovery of sessions/cache is still a device lifecycle
check; the current operation registry is in memory.

linuxd verify_staged_install, bind_pending, install_complete/install_failed and
publication are retained for authenticated staging, UID/deployment registration,
installed remote/commit queries, launch leases, Flatpak deployment metadata,
export/dependency bookkeeping and runtime/extension publication. Flatpak still
expects its deployment scaffold even though the proxy supplies /app payloads
from APKs. No linuxd publication code was removed. Existing app checkouts are
not garbage-collected by this change. The store's legacy base-only APK session
was removed. Metadata/absolute-symlink equivalence between the packed phase-C
snapshot and linuxd's sanitized reconstructed tree still needs device review;
no new sanitization-equivalence claim is made here.

Final stubs declare their launcher activity enabled in the manifest. The host
therefore needs no CHANGE_COMPONENT_ENABLED_STATE grant or cross-package toggle.
On first install the icon can appear just before linuxd binding completes;
V3Launcher must acquire the registered deployment lease before spawning the
payload and fails closed while that registration is absent. An error after a
successful PackageInstaller commit (for example bind_pending failing) is a
post-install registration failure, not a rolled-back APK transaction.

Host validation: run stubgen/tests/run-tests.sh (including signed index manifests,
revision IDs, single-index rejection, retained installed-APK carrier offsets,
base comparison, session planning and fake failures/cancellation),
packs/tests/java-writer.py, java-source.py and run.sh. The filtered
MATON_APPS_ONLY=matonos-linux-subsystem tools/build-apps.sh builds/signs the
whole host app. Host-local AIDL declarations restore the existing private
installer/callback types removed from the bridge; ILinuxdListener mirrors the
canonical declaration in install/linuxd/aidl. No bridge ABI was changed.

Real-PC checks (Ryzen/RX6600, Surface Pro 3, HP Haswell; same protocol on each):
install a small app from the store, inspect its base and single index split,
and launch with /app provided by installed APK carriers. Keep it running while
updating a fixture with unchanged, added, repacked and deleted objects. Compare
unchanged carrier inodes before/after, split revisions and removed split names.
On the current stock PM expect the PID to change when the old index is removed,
regardless of DONT_KILL_APP being requested. Verify apply_extra and runtime/
extension dependencies, unplug/network failure during staging, cancellation
while streaming, PackageInstaller rejection, free-space failure and uninstall.
Repeat with enforcing SELinux, checking installed APK access, /app symlinks and
lease release. Device execution is delegated to the coordinator; none was run
in task 4c2.
