# Chrome extra-data follow-through (chrome-sol)

Status: host install/image publication passes. Chrome's coordinator launch
capture still reports a pinned-commit lookup failure; no VM was started by
this worktree.

## Round 20: trace the portal's pinned deployment identity

Round 19's `/.flatpak-info` pin is `34a83e470564192554ad103904cc56564c876f0032522199c6d3838e091cbf9e`.
The trace shows the extra-data staging checkout at that same commit, and
`StagedPacks.record()` writes the verified `appCommit` and its matching
OSTree commit object into the signed deployment record. PackageDeployment
publishes that record to both the user and default system installation, which
the stock Flatpak portal Spawn lookup can select.

The regression now asks stock Flatpak to resolve the generated app ref and
commit independently through both installation roots. To pinpoint why the
device still rejects Chrome, this round logs the commit and roots received by
portal Spawn and saves both deployment trees plus the running sandbox's
`/.flatpak-info` to `spawn-deployment.txt`. This keeps Flatpak's exact-commit
Spawn behavior intact while exposing whether the device is looking in a
different root or whether its installed record diverges from the launch pin.

The targeted `PackageDeploymentTest` passed all 9 tests, including Flatpak
commit lookup through both roots. `bash -n tests/chrome-sol/verify-device.sh`
passed. The full `tests/run.sh` progressed through pack, placeholder, portal,
and install-handoff checks, then stalled for over four minutes in AOSP D8 while
building the stub-loader test fixture under host JDK 21; that run was
interrupted after its worker threads stopped making progress. The Android
Gradle unit-test task could not run with the host's Gradle 4.4.1
(`dependencyResolutionManagement` is unsupported). No VM was started.

Coordinator: rebuild the subsystem APK and run on a fresh image. The device
script now captures the exact identity and deployment records:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round20 \
bash tests/chrome-sol/verify-device.sh
```

Compare `spawn-deployment.txt`'s active links, deployed refs, and
`/.flatpak-info` commit with the `MatonPortal` lookup log. Do not count install
success as a launch pass; confirm the Chrome window, navigation, close/reopen,
and `--ozone-platform=wayland` in its command line.

## Round 19: publish the post-handback deployment tree

Round 18's install and split publication completed, but Chrome could not find
its default entry point because `/app/extra` was empty at launch. `StagedPacks`
had read the local deployed-tree commit and generated `fresh.entries` before
merging the stub-returned `/app/extra`. It then built the published image and
`objects.index` from that stale entry list. The merged files existed on disk,
but were absent from the mounted EROFS image.

The service now validates and merges the returned payload first, then commits
and reads the full deployed tree. Those refreshed entries are used for the
image and object index, including runtime branch remapping and reuse of
unchanged files. The regression builds/extracts a pre-merge image to show the
payload is missing, then rebuilds from the refreshed tree and checks that both
the Chrome wrapper and `/app/extra/chrome` payload are present. The full host
suite passed at `/mnt/data/aosp/out/pc-logs/chrome-sol-round18-host`.

Coordinator: rebuild the subsystem APK/image and run this on a freshly booted
VM. No VM was started for this change. Verify installation, image/index/
launcher split publication, cleanup of `install/`, a visible Chrome window,
navigation to a page, close/relaunch, and
`--ozone-platform=wayland` in Chrome's process command line. Keep the complete
logcat and screenshot:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round19 \
bash tests/chrome-sol/verify-device.sh
```

## Round 2: exact failure and fix

Rebased chrome-sol onto main 8cd77ce, including the coordinator's removal of
makeReadOnly() from the shared-runtime staging installation. That resolves
the earlier `install/system/repo/tmp.*` create denial.

Attempt 11 does **not** fail on the /usr, /app or /app/extra directory fds:
all three mounts are logged successfully. It fails on the later regular-file
`/etc/e2scrub.conf` bind. Its target was recorded below
`/tmp/oldroot/data/data/.../newroot/etc/e2scrub.conf`, instead of
`/tmp/newroot/etc/e2scrub.conf`. The temporary mount source used the
`/data/user/0` spelling while the kernel proc fd link returned `/data/data`.
Reverse lookup missed the alias and the subsequent reopen saw the unmounted
placeholder inode, triggering bwrap's real dev/ino comparison.

vfs_mount now resolves a native source through O_PATH + its kernel proc fd
link before storing the mapping, as a native bind mount resolves its source.
This handles symlinked ancestors generally, with no app rule or fake stat.
When the native source cannot be opened/read, the existing spelling remains.
Explicit loader --map sources retain their existing symlink semantics.

The revised apply-extra-host.py opens O_PATH fds **inside** the proxy and
passes --ro-bind-fd for /usr, /app and a regular file, plus --bind-fd for
/app/extra. TMPDIR has a symlinked parent, reproducing the device alias case.
Before the fix this produced `Race condition binding dirfd: Invalid argument`;
afterwards it reads the bound file, extracts Debian/xz via bsdtar under the
Flatpak filter, renames output and removes inputs successfully. The payload
also verifies /proc/self remains absent.

## Fixes and host evidence

* Retained proc directory FDs: stock apply_extra sets NO_PROC, but bwrap still
  opens `self` relative to its original proc_fd after pivot_root to finish the
  emulated user mappings. The proxy rebased this FD into the detached old root,
  then failed with `bwrap: open /proc/self failed`. During bwrap setup, resolve
  relative lookups on native proc FDs against their pinned kernel path. This
  uses the real FD, not stale cached names, and does not expose /proc after
  exec into the payload. No Chrome-specific proxy behavior was added.
* Cleanup: older staged shared installations can contain read-only directories.
  Removing children requires writable parent directories; restore owner write
  while deleting the app-owned staging tree. Never follow runtime symlinks.
  Regression checks removal, preserved external runtime, and repeated cleanup.
* apply-extra-host.py runs the shipped musl bwrap under the proxy and a
  Flatpak-shaped libseccomp filter. It asserts /proc/self is absent in the
  payload, extracts a synthetic Debian ar/data.tar.xz with real bsdtar,
  renames the extracted tree, deletes staging inputs, and checks bytes/mode.
  This failed at the retained proc FD before the fix and passes after it.
* tests/run.sh passed, including EROFS extra-data validation, stock OSTree
  pack/delta paths, signed image/index splits, install handoff and launch
  dependency targets. Full Java app/shared/stubgen sources compile against
  SDK 36 with the generated AIDL classes. These are host checks, not Android
  PackageInstaller, Binder service lifecycle or enforcing SELinux tests.

Host commands (logs under /mnt/data/aosp/out/pc-logs/chrome-sol):

```sh
MATON_AOSP_ROOT=/mnt/data/aosp bash tests/run.sh /mnt/data/aosp/out/pc-logs/chrome-sol/host-tests
bash linux/compat-proxy/tests/run.sh
bash linux/compat-proxy/tests/musl-host.sh
python3 linux/compat-proxy/tests/apply-extra-host.py /mnt/data/aosp/out/pc-logs/chrome-sol/apply-extra
```

musl-host.sh prepares the aliases consumed by apply-extra-host.py. Its initial
fixture setup expects no old bwrap/flatpak aliases; remove only those generated
symlinks before rerunning that setup. No downloaded runtime or VM is needed.

## Remaining end-to-end audit and coordinator test

The service builds `/app/` entries from stock `active/files`, including extra,
then ErofsImage builds deployment.erofs. The receiver runs fsck and compares
non-extra entries to the trusted commit; /app/extra is allowed as generated
content under the existing type/mode/symlink checks. The final publication
uses the regular image/index/launcher split path. Cleanup is requested after
StagedPacks verifies the handback. The host cannot prove the Android second
PackageInstaller pass or rendered browser window; check those on the device.

V3Launcher invokes `flatpak run <ref>` with no Chrome command override and
uses HOME=<stub data>/home. Stock Flatpak maps its persistent
`~/.var/app/com.google.Chrome/config` as the app's XDG_CONFIG_HOME. Therefore
chrome-flags.conf belongs there; no per-app launcher rule is needed.
The coordinator must confirm the stock wrapper consumed the flag in cmdline.

On the coordinator's fresh image with this branch's loader and host Java:

```sh
ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol/attempt12 \
bash tests/chrome-sol/verify-device.sh
```

## Round 7: use only syscalls allowed by app seccomp

The device still killed fsck with SIGSYS after timestamp restoration was
removed. The full extraction path also used legacy `chmod`, `mknod`,
`lchown`, `mkdir`, `rmdir`, `lstat`, `unlink`, `link`, and `symlink` calls.
The fsck build now uses the corresponding `*at` interfaces. When checking an
existing extraction root it opens the directory with `O_PATH|O_DIRECTORY|
O_NOFOLLOW` and validates it with `fstat`, preserving the symlink rejection.
Archive timestamps are not restored; validation checks contents, types and
modes.

`linux/packs/tests/fsck-app-seccomp-host.py` generates Bionic's current
x86_64 app filter using `genseccomp.py`, installs that policy in a traced
child, and performs the full extraction of a Chrome-shaped EROFS image with
an executable setuid mode, hardlink, symlink and user xattr. It reports the
first trapped syscall number. It is part of `linux/packs/tests/run.sh`.
Verified after rebuilding with `bash linux/packs/build-mkfs-erofs.sh`.

Coordinator: rebuild the subsystem APK and test on a freshly booted image
(no VM was run for this change). Exact device test:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round7 \
bash tests/chrome-sol/verify-device.sh
```

## Round 8: app-data hardlink denial

Android app-data SELinux denies `linkat` even though the syscall is present in
the app seccomp allowlist. During `fsck.erofs --extract`, repeated EROFS inode
entries are therefore materialized as separate regular files from the image
instead of being linked to the first extracted path. The validator still
checks each committed path's type, mode and SHA-256 against the trusted commit;
hardlink group identity is not part of `/app/extra` validation, where files
are checked for safe types and modes. The pack and OSTree tests continue to
exercise hardlink grouping at deployment creation time.

`fsck-app-seccomp-host.py` now models the Android link denial as `EACCES` for
`linkat`, extracts a Chrome-like image containing a hardlink, and verifies the
copy's bytes and mode without shared inode identity. It also walks configured
build-context ancestors and the standard AOSP checkout location to find
Bionic when `build.sh` sets `MATON_AOSP_ROOT` to an output context. It skips
only when Bionic is genuinely absent. Rebuild with
`bash linux/packs/build-mkfs-erofs.sh`, then run
`MATON_AOSP_ROOT=/mnt/data/aosp bash tests/run.sh <out-dir>`.

Coordinator: rebuild the subsystem APK and test on a freshly booted image
(no VM was run for this change). Exact device test:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round8 \
bash tests/chrome-sol/verify-device.sh
```

The script retries the existing debug install trigger, captures logcat without
clearing others' logs, waits for the actual result, checks installed splits and
install/ cleanup, writes the requested stock flag file, and resolves/starts the
launcher. If it fails, return logcat.txt. Optional TRACE=1 enables full tracing
when the minimal Chrome stub already exists. Then verify the rendered window,
a web page, Wayland flag in /proc/<pid>/cmdline, and close/relaunch. Remove the
cache/v3-trace marker afterwards. Only the coordinator runs VMs.

No shared device-tree, AOSP, kernel or other agents' changes are required.

## Round 2 verification

Logs: `/mnt/data/aosp/out/pc-logs/chrome-sol-bindfd/`.
The x86_64/i386 proxy build and complete proxy tests/run.sh pass, including
Android shared-library packaging, EROFS bind/ldconfig, ICD aliases, fd tracking,
temporary-directory cleanup and rewrite tests. Flatpak-shaped ldconfig/payload
and ICD duplication fixtures pass separately. The revised apply_extra fixture
passes with the real fd bind options and aliased TMPDIR. Device-tree preflight
passes. The complete repository tests/run.sh also passes, including extra-data
validation/cleanup, image/index/launcher splits, install handoff and launch
dependency planning. No AOSP build or VM was started.

The remaining device checks are stock apply_extra against the real Chrome deb,
Binder image handback, commit validation, second PackageInstaller publication,
install/ deletion and an actual Wayland browser launch. Run verify-device.sh
above on a fresh image/APK including commit fb65a0d. Keep the returned log and
check that /etc regular-file bind targets stay in /tmp/newroot rather than
/tmp/oldroot/data/data. If it fails later, return the whole log, not only the
last Flatpak error. No shared changes or kernel changes are requested.

## Round 5: expose the fsck process result

Attempt 14 reaches image creation, then `fsck.erofs` exits nonzero without
output. `ExtraImageValidator` now includes the numeric process status in its
exception and, for Linux's conventional `128+signal` result, prints a possible
signal name. Java's `Process.waitFor()` does not distinguish a signal exit
from a normal exit with the same numeric code, so this is intentionally marked
as possible. The device log from attempt 14 does not include the hidden status
and therefore cannot yet identify why fsck failed.

Host checks: `matonos-mkfs-erofs` and `matonos-fsck-erofs` from
`linux/packs/prebuilt/x86_64/` successfully built and extracted a small image;
the validator regression covers a diagnostic nonzero exit and a SIGTERM exit.
The full `tests/run.sh` suite passes with `MATON_AOSP_ROOT=/mnt/data/aosp`.
These host checks do not reproduce Android app SELinux or its process filter.

Coordinator: build the updated subsystem APK and run the existing fresh-image
test, keeping the full logcat. The failure summary now prints fsck's status and
possible signal inline:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round5 \
bash tests/chrome-sol/verify-device.sh
```

Return the full logcat and the status/signal line. If fsck succeeds, the script
continues with split publication, install/ cleanup and the stock Wayland flag
launch checks described above. No VM was started for this round.

## Round 3: executable on a noexec backing mount

Attempt 12 reaches `execve("/app/bin/apply_extra", ...)` through the proxy and
bwrap reports `EACCES`. The proxy already checks the Linux execute bits, then
`validate_exec()` also called host `faccessat(X_OK)` on the translated backing
path. That second check incorrectly imports Android's backing-mount `noexec`
flag into emulated Linux execution; the payload loader reads and maps the ELF
itself, so the kernel's mount-level exec gate does not apply. Removed that
check while retaining regular-file and execute-bit validation.

`tests/apply-extra-host.sh` recreates the apply-extra bind-fd invocation with
both the script and its `/app/bin/sh` interpreter on a writable `noexec` tmpfs.
It passes the proxy's Flatpak seccomp path and completes Debian/xz extraction
and cleanup. `tests/run.sh` now includes this regression.

Coordinator VM check (no VM was started by this task):

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round3 \
bash tests/chrome-sol/verify-device.sh
```

Keep the full `logcat.txt`. On failure, correlate the `v3 exec resolved` path,
the bwrap `execvp` message, and any SELinux AVCs for the same PID. On success,
continue through image handback, package split publication, stock
`chrome-flags.conf`, and the Wayland launch/render checks in the script.

## Round 4: execute emulated OSTree hardlinks

Attempt 13 did not enable full `--trace` in the final bwrap child. Absence of
`v3 exec resolved` therefore did not prove that execve bypassed the proxy.
The previous host fixture used ordinary copied files; it missed OSTree's
emulated hardlinks. The revised fixture denies host linkat with EACCES and
creates links for both apply_extra and its ELF interpreter before stock bwrap
opens the deployment directory. It also uses `--disable-userns`, as Flatpak
does, alongside bind-fd, noexec backing, and the Flatpak seccomp filter.

Before the loader change, the fixture fails with bwrap's identical Permission
denied message. Full tracing identifies `execve("/app/bin/apply_extra")` returning
-13 after path resolution (`ours 47 -13`; 47 is the proxy's internal syscall
ID). `load_inner()` raw-opened the empty 01600 hardlink placeholder and rejected
its missing execute bits. Payload openat already resolves that name to the
stored 0755 inode. The loader now uses the same hardlink-aware open, for scripts,
ELFs and interpreters. The fixture independently inspects the placeholders
outside the proxy to ensure real hardlinks cannot mask this regression.
Before/after traces: `/mnt/data/aosp/out/pc-logs/chrome-round4-{before,final}/`.

The store's local debug activity uses the subsystem UID, not a generated stub
UID. Android's checkCallingPermission rejects local calls. Store submission now
accepts its own UID explicitly before checking external callers' installer
permission; external stub authentication is unchanged. The device script also
stops on debug-trigger errors rather than waiting forever for an install result.

Coordinator: build the updated loader and host APK into a fresh image, then run:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-round4-device \
bash tests/chrome-sol/verify-device.sh
```

Retry the existing minimal stub so TRACE=1 can place its cache/v3-trace marker.
Capture the full log, including exec resolution, bwrap errors and SELinux AVCs.
On success the script checks publication and cleanup, writes stock
chrome-flags.conf, and launches the resolved activity. Verify a rendered browser,
a web page, the Wayland flag in its command line, and close/relaunch before
calling this complete. No VM was started here. No kernel or shared device-tree
changes are requested; unknown-user getpwuid_r remains a separate warning.

Round 4 validation: complete compat-proxy tests/run.sh passed (including the
Android library build/packaging and revised apply-extra regression); repository
tests/run.sh passed with MATON_AOSP_ROOT=/mnt/data/aosp and SDK 36 tools set.
The changed store service and debug activity compile with javac against SDK 36,
JSON/apksig and the existing generated host/AIDL classes. Device-tree preflight
and git diff --check pass. Logs: chrome-round4-proxy-tests.log,
chrome-round4-suite.log, chrome-round4-preflight.log under out/pc-logs.

## Round 6: app seccomp kills fsck timestamp restoration

The device reported exit status 159 (possible SIGSYS). A `strace -f` run of
static musl `fsck.erofs --extract=...` against a real EROFS deployment showed
`utimensat(..., AT_SYMLINK_NOFOLLOW)` after extracted files and symlinks were
created. The operation restores archive timestamps; Android app validation
only uses contents, object types and modes. Android app seccomp traps this
syscall, so the extraction helper now skips timestamp restoration in its
pinned erofs-utils build. Ownership and permissions remain unchanged, and
mode validation remains active.

`linux/packs/tests/fsck-app-seccomp-host.py` runs image extraction under a
seccomp filter that traps `utimensat`, then checks file contents and executable
mode. It is part of `linux/packs/tests/run.sh`. Rebuild with
`bash linux/packs/build-mkfs-erofs.sh`; the build applies
`linux/packs/patches/0001-fsck-extract-skip-timestamps.patch` to pinned
upstream source. Verified: the regression passed and the full
`MATON_AOSP_ROOT=/mnt/data/aosp bash tests/run.sh <out-dir>` host suite passed.

Coordinator: rebuild the subsystem APK and test on a freshly booted image
(no VM was run for this change). Exact device test:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round6 \
bash tests/chrome-sol/verify-device.sh
```

## Round 9: preserve committed modes during image validation

The `/app` mode mismatch came from `fsck.erofs` inheriting the Android app
process's restrictive umask and applying it during extraction. The EROFS
image carries the correct mode; the temporary extracted tree did not. The
validator now invokes stock fsck with `--preserve-perms`, so validation sees
the image's exact modes regardless of the caller's umask. Mode mismatch
errors include both expected and actual octal modes.

The host regression runs the shipped mkfs/fsck pair under umask `077` and
checks directory and executable modes. The Java validator test also asserts
that `--preserve-perms` is passed and checks the expected/actual diagnostic.
Both the targeted tests and `MATON_AOSP_ROOT=/mnt/data/aosp bash
tests/run.sh /mnt/data/aosp/out/pc-logs/chrome-sol-round9/full-tests` passed.
No VM was started for this change.

Coordinator: rebuild the subsystem APK and test on a freshly booted image.
Exact device test:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round9/device \
bash tests/chrome-sol/verify-device.sh
```

Keep the full logcat. Confirm image validation passes, the second package
install publishes the image/index/launcher splits, install staging is cleaned
up, and Chrome launches with `--ozone-platform=wayland` from its stock
`chrome-flags.conf`.

## Round 12: resolve proxy hardlink modes while building extra-data images

The extra-data image walk read file bytes through `PlaceholderFiles`, but took
the mode and source path from the raw staged entry. Proxy-emulated OSTree
hardlinks are empty `01600` marker files, so image entries inherited that mode.
`ObjectPacks.fromTreeWithModes` now uses the shared `PlaceholderFiles`
resolution for the backing source and its deployed mode; the EROFS writer
continues to read through that same resolver.

The real proxy fixture now compares mode-preserving image entries from an
OSTree hardlink checkout with a copy-mode checkout, then builds and extracts
the image tar and checks every extracted entry's mode. Verified by
`bash tests/placeholder/run.sh /mnt/data/aosp/out/pc-logs/chrome-sol/placeholder`
and the complete
`MATON_AOSP_ROOT=/mnt/data/aosp bash tests/run.sh /mnt/data/aosp/out/pc-logs/chrome-sol/full-tests`
host suite. No VM was run here.

Coordinator: rebuild the subsystem APK and verify on a freshly booted image.
Exact device test:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round12 \
bash tests/chrome-sol/verify-device.sh
```

Confirm image validation passes, the second package install publishes the
image/index/launcher splits, install staging is cleaned up, and Chrome launches
with `--ozone-platform=wayland` from its stock `chrome-flags.conf`.

## Round 13: materialize the verified no-deploy commit for staging

The stub returns the extra-data image before any Flatpak app deployment exists:
the pull used `--no-deploy`. The transaction checks out the already verified
`node.info.commit` into the normal deployment path, verifies the checked-out
metadata against the inspected commit, writes its staging deploy record with
the pulled origin and subpaths, then continues through the usual manifest
validation and second install pass. This keeps the returned extra data separate
while restoring the deployment metadata expected by the existing pipeline.

The host regression reproduced the missing deploy record, recorded the
verified identity, and passed `DeployManifest.build` plus `verifyPlanned`.
`MATON_AOSP_ROOT=/mnt/data/aosp bash tests/run.sh
/mnt/data/aosp/out/pc-logs/chrome-sol/round12-host-tests` passed. No VM was run.

Coordinator: rebuild the subsystem APK and verify on a freshly booted image.
Exact device test:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round13 \
bash tests/chrome-sol/verify-device.sh
```

Confirm the returned image is accepted, the second package install publishes
the image/index/launcher splits, install staging is cleaned up, and Chrome
launches with `--ozone-platform=wayland` from its stock
`chrome-flags.conf`.

## Round 14: compare the deployed `/files` subtree

Attempt 22 returned a valid stub-built EROFS image, then the subsystem rejected
`/app/include` as uncommitted. Flatpak commits contain a top-level `files/`
deployment tree; Flatpak exposes that tree as the deployment's `files/`
directory, which the subsystem maps to `/app` (or to the runtime branch root).
The validator had compared commit-root paths directly with deployment-root
paths. It now projects only committed `/files` entries onto the target root.
Commit metadata and exports are not deployment files, while every path inside
`/files`, including empty directories, remains exact. Extra data is still
allowed only under `/app/extra`.

OSTree inspection of Flathub Chrome stable commit
`34a83e470564192554ad103904cc56564c876f0032522199c6d3838e091cbf9e` showed
the commit root's `/files` tree and its `/files/include` directory and header
files. That commit's parent is `c20cf3297793d37541b82098d83bf2586c347c3b14bad332a5d27df75fe89276`,
the Chrome commit used by attempt 22. This establishes that `/include` is in
the committed deployment subtree, not a path created outside `/app/extra` by
stock `apply_extra`. The host regression builds a deployment containing
`/app/include/header.h`, projects a Flatpak commit fixture containing
`/files/include/header.h`, and validates it; it also confirms `/export` and
`/metadata` are excluded and that a commit without `/files` is rejected.

`MATON_AOSP_ROOT=/mnt/data/aosp bash tests/run.sh
/mnt/data/aosp/out/pc-logs/chrome-sol-round13-host-tests` passed. No VM was run.
Coordinator device check remains:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round14 \
bash tests/chrome-sol/verify-device.sh
```

Confirm the returned image passes commit validation, the second PackageInstaller
pass publishes image/index/launcher splits, staging is removed, and Chrome
launches using `--ozone-platform=wayland` from `chrome-flags.conf`.

## Round 15: read committed modes through proxy hardlink markers

Attempt 23 was rejected because `flextop-init` was reported as mode `0600`
from the downloaded OSTree commit while the returned image had mode `0755`.
The committed-entry reader inspected the repository object path directly. In
the proxy store that path can be an empty `01600` marker, while its indexed
backing inode holds the object and any `user.ostreemeta` metadata. Both the
xattr lookup and the bare-user fallback mode lookup now resolve through the
shared `PlaceholderFiles` implementation before reading metadata. This keeps
the trusted side tied to the OSTree object and prevents marker permissions
from entering `original.entries`.

The regression creates a real bare-user-only OSTree commit containing an
executable, replaces its file object with a correctly indexed proxy marker,
then reads the commit and asserts the mode remains `0755`. The full host suite
passed with `MATON_AOSP_ROOT=/mnt/data/aosp bash tests/run.sh
/mnt/data/aosp/out/pc-logs/chrome-sol/tests`. The suite's final APK check was
updated to inspect the fresh fixture directory produced by its test. No VM was
run.

Coordinator device check remains:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round15 \
bash tests/chrome-sol/verify-device.sh
```

Confirm `/app/flextop-init` validates against the commit, then verify the
second PackageInstaller pass, staging cleanup, and Chrome launch through
Wayland.

## Round 16: validate returned file bytes against logical OSTree contents

`ExtraImageValidator` now hashes each extracted regular file against the
trusted commit entry's source file, read with `PlaceholderFiles.open()`. This
uses the logical OSTree payload when the source path is a proxy-emulated
hardlink marker, instead of treating the marker or an object identity as the
file's byte digest. Mismatch diagnostics include expected and actual SHA-256
values. The validator's other checks inspect extracted image metadata or
symlink targets and do not read regular-file contents outside this helper.

The host regression uses an indexed `01600` marker whose backing inode contains
a header file, validates matching image bytes, then rejects tampered bytes. The
full host suite passed with `MATON_AOSP_ROOT=/mnt/data/aosp bash tests/run.sh
/mnt/data/aosp/out/pc-logs/chrome-sol-round16-host`. No VM was run.

Coordinator device check remains:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round16 \
bash tests/chrome-sol/verify-device.sh
```

Confirm the Chrome header validates, then verify the second PackageInstaller
pass, staging cleanup, and launch through Wayland.

## Round 17: create Flatpak's per-app runtime lock file

The round-16 trace shows the second bwrap does not block on an info/sync fd.
It completes the pivot, then its command child exits immediately: bwrap logs
`execvp chrome: No such file or directory`. Before that, Flatpak's stock
runner binds `/run/flatpak/per-app-dirs-ref` from
`$XDG_RUNTIME_DIR/.flatpak/com.google.Chrome/.ref`; the proxy trace maps this
to the stub runtime directory, where the backing file was absent. Flatpak
uses this empty file as the per-app directory lock while starting bwrap.

`PerAppRuntime` now creates the empty mode-0600 lock file from the validated
application ref. This is generic to Flatpak app IDs and stays in the stub's
private runtime directory. A Java host test checks its path, type, mode and
idempotence. A bwrap/proxy regression then reproduces Flatpak's bind,
`--lock-file` and `execvp chrome` sequence; the command reads the lock file and
exits successfully.

Host checks:

```sh
MATON_AOSP_ROOT=/mnt/data/aosp bash tests/run.sh /mnt/data/aosp/out/pc-logs/chrome-sol-round17-host
bash linux/compat-proxy/tests/run.sh
```

The compat-proxy suite now runs the bwrap launch regression after building its
static musl bwrap fixture. No VM was run for this fix. Coordinator device check
on a freshly built image:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round17 \
bash tests/chrome-sol/verify-device.sh
```

Verify the lock bind no longer produces `ENOENT`, Chrome opens a Wayland
window, and `/proc/<Chrome PID>/cmdline` contains
`--ozone-platform=wayland`. Keep logcat running while opening a page and
closing/relaunching Chrome.

### Round 29: window-state dispatch callback storm

The supplied day-r33 kill log already contains the decisive descriptor dumps:
Kate UID 10130 reaches 6000 proxies (04:20:22.408), histogram
`android.os.IRemoteCallback x5932`; Chrome UID 10166 reaches 6000
(04:21:29.320), histogram `android.os.IRemoteCallback x5965`.
These are both stub UIDs, not isolated GPU children. AMS's native per-sender-UID
watermark callback starts an asynchronous dump/kill thread; five-second count
samples do not exclude an intervening burst.

`StubActivity.applyState` treated every state other than minimize/capture as
fullscreen-off, including decoration (6/7) and geometry (16+). Its public
`requestFullscreenMode` call allocates an IRemoteCallback (see AOSP
FullscreenRequestHandler); the confirmation configures the Linux window again.
Only explicit fullscreen-on/off events now reach that path. No quota change,
timer, per-app rule, or AOSP modification is needed.

`python3 tests/chrome-sol/window-state-dispatch.py` extracts and compiles the
production dispatch method with counting stand-ins: 70,000 decoration/geometry/
unknown notifications create zero fullscreen callbacks; enter and exit still
invoke their callbacks. This runs in tests/run.sh.

Coordinator device verification (no VM run by this agent): fresh image/APK,
untraced Kate and Chrome, collect full logcat through startup and 140 seconds.
Sample `dumpsys activity binder-proxies` every second for the first 20 seconds,
then every five seconds; retain the complete dump, UID mapping from
`cmd package list packages -U`, and any AMS descriptor histogram. Check neither
stub is killed, counts stay bounded by live windows, and opening/resizing/closing
windows plus fullscreen enter/exit work. Specifically confirm no repeated
fullscreen transitions from decoration changes. If another quota breach occurs,
retain `Too many binder proxy objects`, `Uid ... sent too many`, and the following
`BinderProxy descriptor histogram` together; a low later sample is insufficient.

Exact collection command (coordinator's VM serial, packages already installed):
`bash tests/chrome-sol/verify-binder-peak.sh localhost:5556 /mnt/data/aosp/out/pc-logs/chrome-r29-device`.
This disables each stub's trace marker, launches each app separately for 140 s,
records UID mappings/counts/full logcat, and force-stops that app afterward.

### Round 30: Chrome HOME operations and process-tree lifetime

The new HOME link store did not reproduce the reported blank Chrome window on
this host. Chrome 156.0.8078.47 launched through the proxy with a marked HOME,
created its profile and Singleton symlinks, and rendered the headless
`about:blank` document. The host regression also runs the real `HomeLinkStore`
setup under a kernel filter that denies `linkat`, then exercises Chrome-like
Singleton symlinks together with linked Crashpad data through create, unlink,
rename and read. This validates those operations, but it does not establish the
cause of the device-only blank window; keep the device proxy trace and inspect
failed `linkat`, rename, open and symlink operations beneath the app HOME if it
persists.

The Android stub waits on the `--supervise` loader process. That subreaper's
exit is the end-of-session signal after its main loader and adopted Linux
payload descendants have exited. It no longer treats Flatpak's first app
process exiting during a restart as the end of Chrome. The supervisor host
test makes the parent exit while a detached child holds a `window-open`
sentinel; the Android-visible session remains alive until that child removes
the sentinel and exits.

Host checks for this round:

```sh
bash linux/compat-proxy/tests/home-links.sh
python3 linux/compat-proxy/tests/supervise.py \
  linux/compat-proxy/build/linux-layer-v3 \
  linux/compat-proxy/build/linux-layer-v3-i386
MATON_BUILD_ROOT=/mnt/data/aosp MATON_AOSP_ROOT=/mnt/data/aosp \
  MATON_SUBSYSTEM_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-r30-android-compile \
  MATON_ANDROID_JAR=/mnt/data/aosp/prebuilts/sdk/current/public/android.jar \
  MATON_AAPT2=/mnt/data/aosp/out/host/linux-x86/bin/aapt2 \
  MATON_COMPOSITOR_JNI_LIBS=/mnt/data/aosp/out/pc-logs/chrome-sol-round22/deps/compositor/jniLibs \
  MATON_V3_ANDROID_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round22/deps/linux-subsystem-app/loader \
  MATON_PIPEWIRE_BUILD=/mnt/data/aosp/out/pc-logs/chrome-sol-round22/deps/pipewire \
  MATON_SIGNING_STORE_FILE=/mnt/data/aosp/out/pc-logs/chrome-sol-r30-android-compile/no-signing-used.jks \
  MATON_SIGNING_KEY_ALIAS=unused MATON_SIGNING_STORE_PASSWORD=unused \
  MATON_SIGNING_KEY_PASSWORD=unused ANDROID_HOME=/home/hanro50/Documents/matonos-android-sdk \
  ANDROID_SDK_ROOT=/home/hanro50/Documents/matonos-android-sdk \
  /home/hanro50/Documents/gradle-9.4.1/bin/gradle -p linux/subsystem/host \
  --no-daemon --project-cache-dir /mnt/data/aosp/out/pc-logs/chrome-sol-r30-android-compile/gradle-cache \
  -PrealVersionCode=9999 :app:compileDebugJavaWithJavac -x :app:preBuild
```

Coordinator device check (no VM run by this agent), including the Chrome
self-relaunch after accepting the first-run dialog:

```sh
TRACE=1 ADB_SERIAL=127.0.0.1:5556 \
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol-round30 \
bash tests/chrome-sol/verify-device.sh
```

Confirm Chrome renders before and after accepting the dialog, the replacement
browser process stays alive after its parent exits, and its command line keeps
`--ozone-platform=wayland`. Also check that the stub stays alive until the last
Chrome payload process exits. Capture `logcat.txt`, `spawn-deployment.txt`, the
screenshot and the HOME link-store files if the device-only blank window
continues. No VM, kernel change, or shared-tree change was made.

Round 29 host results: production-dispatch regression PASS; full tests/run.sh
PASS; Android :app:assembleRelease PASS; signed APK dependency closure (81
libraries) and bootstrap package/embedded APK checks PASS. Build output:
`/mnt/data/aosp/out/pc-logs/chrome-sol-round29/`; build log:
`/mnt/data/aosp/out/pc-logs/chrome-sol-round29-build.log`. Device-tree preflight
PASS. No device run, kernel change, shared-tree change, or push.
