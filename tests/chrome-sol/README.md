# Chrome extra-data follow-through (chrome-sol)

Status: host fixes and tests complete; device install/launch is NOT verified.
Attempt 11 reaches apply_extra; its bind-fd failure is reproduced and fixed on the host.
No VM was started and no other worktree was modified.

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
