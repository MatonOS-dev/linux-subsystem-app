# Flatpak Software Centre source

`FlatpakSourceService` exports `org.matonos.store.SOURCE` under
`org.matonos.permission.BIND_STORE_SOURCE`. All three AIDL files are copied
byte for byte from `store-sources/contract/aidl/org/matonos/store/source/`
in the device tree (contract version 5). `commit` has no `inherit` argument: the store
infers full vs inherited sessions from whether `base.apk` is supplied. The
store owns package installation. The host now targets and requires API 37, as
requested for this source.

Catalogue data comes from Flatpak's `update --appstream` path and the remotes'
`appstream/<remote>/x86_64/active/appstream.xml[.gz]`. There is no Flathub web
catalogue client. The service streams an AppStream catalogue file to the store;
the store indexes and searches it locally. Details read AppStream text,
screenshots, licence, developer, release, homepage and OARS attributes.
The existing native inspection helper now uses libappstream for territory age
mapping, and Flatpak's public API for exact download/installed byte sizes.
Permissions come from Flatpak remote metadata. Cached icons are exposed through
a read-only content provider requiring the same store permission.

IDs are `remote::app/id/arch/branch`, so origin and branch selection survive a
search/details/install round trip. Unqualified app IDs/full refs are accepted
only when unambiguous. Flathub is the initial preset; removing it is persistent
and does not silently re-add it on the next service start. Added HTTPS
flatpakrepo descriptors are handled by Flatpak. Staging exports each remote's
GPG key as well as its URL. Changing the origin of an installed Android carrier
requires uninstalling it first. Broken AppStream refreshes are logged and
reported in repos; available cached metadata can still be read.

Flatpak does not provide third-party user ratings or reviews; the store owns ratings through its generic service.

## AppStream summary-index failures

The pinned Flatpak 1.19.2 reads `summary.idx` before the full OSTree summary.
If it cannot fetch or verify the index, its remote state has no refs, so the
AppStream refresh reports both `appstream2/<arch>` and `appstream/<arch>` as
missing even when the full `summary` lists them. This was reproduced with the
shipped Flatpak on the host; the system Flatpak 1.14.6 completed the same
FlatPark refresh. The default-remote seed marker also meant an already-seeded
installation never imported newer signing keys from an updated descriptor.
Default descriptor keys are now imported at service initialization. This
refresh is generic for built-in descriptors and does not special-case a remote's
catalogue contents.

Catalogue refresh and XML parsing are isolated per enabled remote. A failed
refresh uses readable cached XML and leaves that repository marked broken; a
missing or invalid XML file is logged with the remote and skipped. If every
remote is unavailable, the catalogue can be empty, while
installed listings retain their signed package records. The host test covers a
mixed healthy/broken set and the all-broken case.

## Installed refs and real updates

One Android package represents a Flatpak app id: `flatpak.` plus the app id,
with the id's dots kept. Architecture and branch are represented by one split
named `<arch>.<branch>`. Android-invalid characters map deterministically to
`_`, and identifier segments that do not start with a letter get a leading
`v`. The signed deployment split contains that ref's record, object indexes,
and EROFS image. The base APK carries the reusable stub runtime; launcher
aliases carry their exact Flatpak ref in split metadata. If two distinct app
ids encode to the same Android package name, installation fails with a
collision error.

Installed-ref queries read all signed deployment splits in each package and
create a disposable Flatpak metadata installation from those verified records;
payloads remain in their APK images. Remotes are registered after materializing
the records so their key configuration is retained. Subpath deployment names
are preserved. Missing deployment state is treated as empty; old-format stubs
are not migrated. The native helper calls
`flatpak_installation_list_installed_refs()` or
`flatpak_installation_list_installed_refs_for_update()`; the latter resolves a
Flatpak update transaction without installing it. The plugin does not implement
its own version/commit update comparison. Incomplete signed stubs are logged
and omitted by the existing installed-package reader.

## Install and commit ownership

Each operation has leased private staging under WorkDirs and a foreground
notification/wake lock. A public-API Flatpak transaction resolves the requested ref, dependencies,
related extensions and subpaths and downloads with `no_deploy` enabled.
The helper returns the transaction's exact refs/origins/commits/subpaths.
Verified commits are checked out without running deploy hooks or `apply_extra`.
OSTree fsck runs before StagedPacks and StubGenerator produce the signed base,
the requested `<arch>.<branch>` deployment split, and its launcher split.
Android carrier dependency edges are
constructed only after Flatpak has selected the deployment set; they do not
select/download dependencies. App carriers reference the selected transaction;
runtime parents reference their selected extension carriers.

`CommitChain` validates and orders the carrier graph. `StoreCommitInstaller`
passes read-only APK descriptors to the store and waits for its terminal
`ICommitResult` before allowing dependants. A failed runtime stops the chain,
so the application is never submitted. Store Binder death releases a waiter.
An update uses PackSessionPlan to replace the matching branch split while
retaining other branch splits and the reusable base. Each changed package has
its own store-owned commit.
This path never creates PackageInstaller sessions or uninstalls a package.
Artifacts remain leased until terminal results and are removed afterwards.
Successful dependencies remain installed if a later package fails; the source
cannot roll them back because the store owns installation/uninstallation.

## Local `.flatpak` bundles

The source contract adds `installLocalFile(in ParcelFileDescriptor file,
String displayName, boolean allowUnsigned, String operationId,
IInstallCallback callback)`. The caller supplies an already-open local bundle
descriptor; the source copies it with overflow-checked byte accounting into
the operation's leased `WorkDirs` tree. The display name is diagnostic only.
Cancellation, progress, commit ownership and terminal callbacks use the same
operation lifecycle as a repository install. `.flatpakref` is not accepted by
this entry point.

The native plan helper adds the bundle to a stock `FlatpakTransaction` with
`flatpak_transaction_add_install_bundle()` and runs the transaction with
`no_deploy`. Flatpak itself selects the bundle app, runtime, RuntimeRepo
origin, related refs and commits. Those operations feed the existing
`flatpak.<app-id>` carrier and `<arch>.<branch>` split pipeline. A bundle
without a detached commit signature is refused unless `allowUnsigned=true`;
that argument is intended to be set only after the store's unverified-bundle
warning. No APK commit is submitted before this check. The source reports the
result to the store as a progress phase exactly `Local bundle signature:
signed` or `Local bundle signature: unsigned`; `done`/`failed` retain their
existing terminal meaning. Repository installs keep their existing
verification path.

## Extra-data through the store

Extra-data metadata selects a real stub base declaring ExtraDataInstallService;
there is no temporary self-install on this source path. Runtime/extension
carriers commit first. The store then commits the app base and normal splits
and returns success before the subsystem binds the installed stub.

Only that stub's UID executes stock Flatpak deploy/`apply_extra`. Protocol 3
returns an unsigned EROFS APK split; protocol 1 remains supported by the stub
protocol implementation. Remote descriptors retain their GPG keys. The subsystem extracts the
returned image and runs the existing ExtraImageValidator against the original
verified OSTree commit, respecting Flatpak's selected subpaths. Content outside
`/app/extra` (or the runtime branch’s `/extra`) must match the commit. The subsystem regenerates/signs the APK
envelope from validated image bytes, so a returned manifest is never trusted.
It rebuilds and submits the same `<arch>.<branch>` split with the validated
image and original signed deployment metadata. No base or other branch split
is sent in this commit; the store retains those installed splits. Extra-data
is regenerated on subsequent updates rather than used as an OSTree delta base.

CommitChain waits for both terminal results before advancing to dependants or
sending done. A rejected stub, failed apply_extra, invalid image or rejected
split stops the chain and sends failed. A validation failure submits no second
commit. The stub's build directory is cleaned after validation/signing. An
already accepted stub or dependency remains installed after failure; only the
store can uninstall it. An interrupted/failed extra-data install must be retried
from the store; no automatic rollback or background completion is claimed.

## Validation on 2026-11-15 (round 2)

- `test-store-source.sh`: passes AppStream parsing, five-package Steam dependency
  ordering and failed-runtime prevention, plus extra-data stub → apply_extra →
  validation → split-only commit → done. Failed validation sends failed and
  never submits the split. These are host scheduling models, not device tests.
- The changed C helper builds against the prepared AppStream prefix.
  `test-store-flatpak.sh` passes with a tiny local unsigned repository: an
  actual Flatpak transaction selects/downloads both app and runtime without
  deploying either; installed refs, exact sizes and real before/after updates
  still pass, including a metadata-only installed snapshot.
- The full repository host suite passes, including existing EROFS validation,
  extra-data handback, placeholder reads and signed split tests.
- The AIDL copy matches the canonical directory byte for byte.
- Release Java/AIDL compilation and APK assembly pass at compile/min/target API
  37. Both signed APKs are produced. Signature verification, zip alignment,
  native dependency closure (82 libraries) and bootstrap embedded-APK checks
  pass. The final full-build ISA audit exits 1 with 550 findings, all in the
  unchanged bundled `lib/x86_64/libmatonos-flatpak.so` multicall prebuilt:
  AVX instructions such as `vmovdqa` in C++ functions, and `pclmulqdq` in
  `lzma_crc32`, exceed the audit's x86-64-v2 baseline. No audit suppression,
  packaging exclusion or pickFirst workaround was added. These APKs are build
  artifacts for follow-up testing, not a passing generic-PC release build.
  The new AppStream/transaction helper is not among the reported failures.

Artifacts and evidence are under
`~/matonos/build/flatpak-source2-out/subsystem/`: `MatonLinuxSubsystem.apk`,
`MatonLinuxSubsystemBootstrap.apk`, `round2-build.log` (full workflow),
`round2-native.log`, and `round2-package.log` (repackage the final FD lifecycle
logging changes after the already-passing host suite). Local Flatpak staging
and update evidence is `../round2-integration.log`.

Build uses `GRADLE_USER_HOME=~/matonos/build/flatpak-source2-out/gradle`,
`MATON_SUBSYSTEM_OUT=~/matonos/build/flatpak-source2-out/subsystem` and the
normal `./build.sh` signing/SDK environment from `tools/build-subsystem.sh`.
The wrapper itself is not invoked because its final staging step would write
the shared device tree. The output was seeded with `rsync -aS main-out/ OUTPUT/`;
rsync reported permission-denied files in old test fixtures (exit 23), while
copying the dependency builds successfully. The AppStream dependency prefix
was additionally copied read-only from flatpak-update-out into this output.
`MATON_ZINK_STAGE` and `MATON_ZINK_STAGE_I386` select the existing prepared
Zink stage trees; they are only read. The helper build now exports the prefix
pkg-config search path for both of its isolated container invocations.

## Device checks still required

1. Install a complete API-37 subsystem APK and bind from the permitted store.
   Check discovery, basic search, details, cached content-provider icons, OARS,
   permissions, and store-owned ratings. A caller without the bind permission must
   not bind or open the icon provider.
2. Add a signed HTTPS remote, select an origin-qualified app, install, query
   installed/updates, remove the remote, and check cached/offline behaviour.
3. Install an app with a runtime and several extensions. Record each callback
   commit and its terminal result; every dependent must wait. Reject a runtime
   session and confirm no application session is submitted and failed is sent.
4. Exercise Steam or another extra-data app. Record the accepted base/normal
   split commit before the stub's apply_extra starts, then the validated
   split-only EROFS replacement (no base), its result, and done. Confirm exactly
   one EROFS image remains and launch can read `/app/extra`. Corrupt the returned
   image outside `/app/extra`: expect failed, no second commit and no dependants.
   Retry an interrupted first or second phase. Test custom signed remotes,
   subpath installs, stub process death and an extra-data runtime/extension;
   these Binder/device interactions are not covered by the host models.
5. Update an installed app: check inherited unchanged splits, replacement and
   removal of obsolete splits, launcher visibility, retained app data, and
   successful launch. Check shared-runtime updates across multiple apps.
6. Kill the store while awaiting a commit; confirm an error log, operation
   cleanup and no further dependent commit. Check parallel operations involving
   shared runtimes for store-owned revision conflicts.
7. Exercise large unfiltered catalogue transfer through the read-only fd and
   verify that the store imports it incrementally into SQLite/FTS.

No image builds, VM boots, main-worktree changes, AOSP patches, kernel changes,
or shared device-tree changes were made. Download staging currently downloads
a complete dependency closure even for Android-carrier updates; object-cache
reuse is a future performance improvement.

## Browse collections and ratings

The revision 2 source method supplies `popular` and `new` from Flathub's
collection API. Results join against configured Flathub AppStream refs so
installs retain the remote-qualified identity. Collections, search results, and details contain no user rating data. Ratings
are fetched and submitted by the Software Centre through its single generic
ratings service, independent of source. Flatpak no longer uses ODRS for ratings
or reviews; Flathub verification badges and Popular download scores remain.


## Store catalogue v5

The Flatpak source emits an UTF-8 JSON catalogue file from its AppStream index and returns a read-only file descriptor. It includes the repository candidate, installed/update flags, AppStream added date, Flathub popularity score, signer/installable state, and repository display/priority metadata. The source generation changes on repository configuration or package changes and when the catalogue refreshes after six hours. Search and browse queries belong to the Software Centre's SQLite/FTS index.

The existing source host suite still covers AppStream parsing and Flatpak source support helpers. A fresh Software Centre image is still needed to verify Binder descriptor transfer and on-device query/import behavior.
