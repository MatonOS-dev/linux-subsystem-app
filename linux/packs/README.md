# OSTree APK packs (format v1)

Packs provide the Linux app payload through installed, immutable APK carriers.
The base APK contains the stub and its UI/permission metadata. A code-free index
split contains `objects.index` and `objects.ostree`; code-free content splits
contain the file payloads. The compat proxy mounts `/app` directly from indexed
APK byte ranges without extracting the payloads.

## Object format

`objects.ostree` starts with `MTPKOBJ1`, a little endian u32 object count, then
records sorted by canonical checksum: 32 raw checksum bytes, little endian u64
object length, and the exact raw libostree object bytes. It retains the commit,
reachable dirtrees and dirmetas unchanged.

`objects.index` starts with `MTPKIDX1`, a little endian u32 entry count, then
records in UTF-8 path order. Each record contains five u32-length-prefixed strings
(path, canonical checksum, pack filename, kind, symlink target), a u32-length-
prefixed xattr byte string, then little endian u64 payload offset and length,
and u32 uid, gid and mode. Kinds are `dir`, `file` and `symlink`. Only files
have pack payloads. Checksums identify canonical OSTree objects, rather than
payload bytes alone. Xattrs retain the serialized `a(ayay)` variant.

Content ZIPs use STORED entries named by canonical file-object checksum.
Private local-header extra fields align payloads to 4096 bytes. Their names
are `pack-<sha256-of-original-zip>.zip`; APK split names are `pack_<sha256>`.
The manifest and signature are appended without moving indexed payloads, so
the split identifier hashes the original ZIP rather than the signed APK.
Initial packing groups objects by parent directory with a 64 MiB size target.

`reader.c` is the bounded C99 lookup/stat/readdir reader; the compat proxy uses
its buffer interface with its own allocator. `pack.py` is the host reference
writer using PyGObject/libostree. `ObjectPacks` is the Java writer.

## Source and publication

`OstreePacks` resolves revisions using stock `ostree rev-parse`, reads verified
binary commit/dirtree/dirmeta GVariants, and obtains file metadata with
`ostree ls --dironly --checksum --xattrs`. It supports bare-user and
bare-user-only repositories; paths come from binary dirtrees, avoiding CLI
path escaping. Metadata enumeration currently costs one CLI invocation per file.

`StagedPacks` runs the packaged v3 loader's `matonos-flatpak ostree` selector.
After Flatpak completes, including apply_extra, it commits the deployment's
`files/` into a separate operation-local bare-user repository and packs that
snapshot. It never changes the download repository's mode or publishes this
private commit. Index paths are relative to `/app`, including `/extra`.

linuxd still authenticates staging, registers UID/deployment identity, maintains
launch leases, publishes deployment metadata and handles exports/dependencies,
runtimes and extensions. Its separately sanitized deployment is not assumed
byte-equivalent to the packed snapshot; that trust boundary remains a device
integration concern. Publication and existing checkouts remain in place.

## Add-only content updates

Index splits are named `index_r<revision>_<sha256>`, with a canonical positive
decimal revision bounded by Java's signed int range. The digest hashes both
index files, each prefixed by its eight-byte big endian length. The revision
also becomes the APK revisionCode. New splits use one plus the highest installed
base/split revisionCode; inherited splits retain their revisions. Exhaustion
fails explicitly. Identical newest index contents reuse the installed index.

Stub versionCode 18 identifies this template and naming format. Base and all
splits have that same versionCode. Older templates, or changed base contents
(including permissions/desktop metadata), require a full install. Ordinary
content updates keep versionCode fixed and use MODE_INHERIT_EXISTING with
setDontKillApp(true), no base, no replacements and no removeSplit calls. They
only write a new index and missing/repacked carriers; all installed splits
remain inherited, including old indices and carriers needed by running apps.
Stock PackageManager preserves the requested flag for this additive case.

`PackLaunch` selects the highest index revision from PackageManager's parallel
name/path arrays. Every index name must parse, and any revision tie fails
closed. There is no base-index fallback. It materializes only that index and
links only its referenced carriers into a revision-content cache directory.
The loader pins carrier descriptors before executing the payload; an existing
process keeps its original mappings while a later launch uses the newest index.

The production repack threshold is 0.5 dead payload bytes divided by total
payload bytes across carriers the new index will reference. The denominator
includes newly added carriers. Retained ZIP directories supply the complete
object inventory, including objects dropped by earlier indices. Entirely dead
carriers and carriers belonging only to superseded indices are excluded.
At the threshold, retained live objects are repacked; otherwise their immutable
carriers are inherited. Python and Java use the same policy.

## Deferred collection

GC is a separate removal-only MODE_INHERIT_EXISTING session. It removes every
superseded index and each carrier the newest index does not reference, keeping
the base and newest index untouched. Its normal kill flag is intentional:
stock PackageManager clears DONT_KILL_APP for split removals regardless.

The conservative trigger is the next store update while the package is already
explicitly disabled (DISABLED or DISABLED_USER) and ActivityManager reports no
process for its UID. REAL_GET_TASKS grants the host process visibility; missing
permission or unavailable process state defers collection. Disabled state blocks
normal launches during this window. The store never disables, enables or stops
an app for collection, and checks eligibility again through session submission.
An external administrator must not re-enable the package during maintenance.
Enabled packages retain old generations until they meet this trigger.

GC completes before the independent content session. Its asynchronous result
refreshes PackageManager state before content planning. A failed committed GC
leaves collection deferred; content installation can continue. Cancellation
before commit abandons the session. Once committed, the store waits for the
actual result. The in-memory operation registry does not provide process-death
recovery; no enabled-state restoration is needed because GC never changes it.

Tests: `linux/stubgen/tests/run-tests.sh`, `linux/packs/tests/java-writer.py`, `linux/packs/tests/java-source.py`, and `linux/packs/tests/run.sh` cover the host format, signing, planner and reference implementations.
