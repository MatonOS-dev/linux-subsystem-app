# Flatpak Software Centre source

`FlatpakSourceService` exports `org.matonos.store.SOURCE` under
`org.matonos.permission.BIND_STORE_SOURCE`. All three AIDL files are copied
unchanged from the version-1 store contract. The legacy `ILinuxAppStore`
implementation and its installer are unchanged. The host now targets and
requires API 37, as requested for this source.

Catalogue data comes from Flatpak's `update --appstream` path and the remotes'
`appstream/<remote>/x86_64/active/appstream.xml[.gz]`. There is no Flathub web
catalogue client. Searches return basic listings; details read AppStream
text, screenshots, licence, developer, release, homepage and OARS attributes.
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

Reviews use ODRS's fetch endpoint, with the bundle's application ID, a
40-character read-only anonymous client hash, locale, version and unlimited
review request (`limit=-1`). ODRS may apply its own server policy. Host validation
retrieved 451 Steam reviews. Empty versions must be sent as `unknown`: the
service rejects an empty version. Reference: https://odrs.gnome.org/ and the
GNOME ODRS fetch implementation.

## Installed refs and real updates

Android's signed index split is the persistent deployment record. Querying
creates a disposable Flatpak metadata installation from those verified records;
payloads remain in their APK images. Remotes are registered after materializing
the records so their key configuration is retained. Subpath deployment names
are preserved. The native helper calls
`flatpak_installation_list_installed_refs()` or
`flatpak_installation_list_installed_refs_for_update()`; the latter resolves a
Flatpak update transaction without installing it. The plugin does not implement
its own version/commit update comparison. Incomplete signed stubs are logged
and omitted by the existing installed-package reader.

## Install and commit ownership

Each operation has leased private staging under WorkDirs and a foreground
notification/wake lock. Stock Flatpak installs the requested ref into this
private directory, resolving dependencies and related extensions itself.
OSTree fsck runs before the existing StagedPacks and StubGenerator produce
signed base/index/EROFS/launcher APKs. Android carrier dependency edges are
constructed only after Flatpak has selected the deployment set; they do not
select/download dependencies. App carriers reference the selected transaction;
runtime parents reference their selected extension carriers.

`CommitChain` validates and orders the carrier graph. `StoreCommitInstaller`
passes read-only APK descriptors to the store and waits for its terminal
`ICommitResult` before allowing dependants. A failed runtime stops the chain,
so the application is never submitted. Store Binder death releases a waiter.
An update uses PackSessionPlan's inherit and removeSplits decisions; unchanged
packages need no session. Each changed package has its own store-owned commit.
This path never creates PackageInstaller sessions or uninstalls a package.
Artifacts remain leased until terminal results and are removed afterwards.
Successful dependencies remain installed if a later package fails; the source
cannot roll them back because the store owns installation/uninstallation.

## Validation on 2026-11-15

- `bash linux/subsystem/host/test-store-source.sh OUTPUT`: passed. Plain Java
  tests cover gzipped AppStream, app-vs-runtime bundles, fallback language,
  descriptions, developer, screenshots, OARS and basic listing shape; the
  five-package Steam model covers dependency ordering, base plus splits,
  inherit/removeSplits, success, failed runtime preventing Steam commit, and
  graph validation before the first commit.
- The native helper compiled and linked against the prepared Flatpak/AppStream
  musl dependencies mounted read-only. `--oars oars-1.1 en_US
  violence-cartoon=mild` returned age 3 / ESRB / Early Childhood.
- `bash linux/subsystem/host/test-store-flatpak.sh MULTICALL HELPER OUTPUT`:
  passed against a tiny local repository. It verifies actual Flatpak runtime
  resolution, full app/runtime refs, positive exact sizes, no update before
  publishing and a real update afterwards, including a payload-free snapshot.
  The fixture is explicitly unsigned and local. Host export triggers log the
  unavailable image-specific bwrap path; the test checks metadata/resolution,
  not desktop integration or launch.
- `:app:compileDebugJavaWithJavac -x :app:preBuild` with compile/min/target API
  37: passed. This validates the Java and generated AIDL, not APK packaging.
  Gradle uses `~/matonos/build/flatpak-source2-out/gradle` exclusively.
- Full `:app:assembleRelease` attempted with the dedicated external output.
  Blocked at `:app:buildFlatpakPlan`: the offline builder reports
  `ninja: chdir: No such file or directory` for `/scratch/work/build/flatpak`.
  Its default host input is
  `~/matonos/build/musl-324/flatpak-musl/build/flatpak`, which is absent.
  Prepared dependencies exist in the other task's flatpak-update-out directory;
  they were mounted read-only for the separate helper compile, rather than
  rebuilding/mutating that task's outputs. No APK was produced and no packaging
  exclusion/pickFirst workaround was added.

Reproduce the Java check with the signing variables and SDK configured as in
README, dedicated GRADLE_USER_HOME/MATON_SUBSYSTEM_OUT, and
`gradle -p linux/subsystem/host -PrealVersionCode=<valid code>
:app:compileDebugJavaWithJavac -x :app:preBuild`.
For a complete APK, first prepare the pinned dependencies for that output with
the repository build workflow, then run the normal build without task skips.
The plugin also requires the newly compiled inspection helper in the APK;
a Java-only incremental deployment with the old helper is insufficient.

## Device checks still required

1. Install a complete API-37 subsystem APK and bind from the permitted store.
   Check discovery, basic search, details, cached content-provider icons, OARS,
   permissions and ODRS rendering. A caller without the bind permission must
   not bind or open the icon provider.
2. Add a signed HTTPS remote, select an origin-qualified app, install, query
   installed/updates, remove the remote, and check cached/offline behaviour.
3. Install an app with a runtime and several extensions. Record each callback
   commit and its terminal result; every dependent must wait. Reject a runtime
   session and confirm no application session is submitted and failed is sent.
4. Exercise Steam or another extra-data app. This source uses stock Flatpak's
   deploy/apply_extra through the host compat loader; it does not reuse the
   legacy installer's temporary self-installed stub. Confirm bwrap, runtime
   mounts and extra-data execution work in this path. This has NOT been tested
   on a device and is a material integration risk, not covered by the modeled
   Steam commit test.
5. Update an installed app: check inherited unchanged splits, replacement and
   removal of obsolete splits, launcher visibility, retained app data, and
   successful launch. Check shared-runtime updates across multiple apps.
6. Kill the store while awaiting a commit; confirm an error log, operation
   cleanup and no further dependent commit. Check parallel operations involving
   shared runtimes for PackageInstaller revision conflicts.
7. Test large unfiltered catalogues for Binder payload suitability. The v1
   contract has no pagination; this source adds no arbitrary result cap.

No image builds, VM boots, main-worktree changes, AOSP patches, kernel changes,
or shared device-tree changes were made. Download staging currently downloads
a complete dependency closure even for Android-carrier updates; object-cache
reuse is a future performance improvement.
