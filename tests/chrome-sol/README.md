# Chrome extra-data follow-through (chrome-sol)

Status: host fixes and tests complete; device install/launch is NOT verified.
The original device tempfile denial is not yet identified to a pathname.
No VM was started and no other worktree was modified.

## What the supplied logs establish

Attempt 9, PID 3256, ends with `While trying to apply extra data: Creating
 temp file: Permission denied` (log lines 3107–3109). Flatpak's
`apply_extra_data()` calls `flatpak_run_setup_base_argv()` before spawning
bwrap. Its propagated error has this shape; a failed spawned script instead
returns `apply_extra script failed, exit status N`. The exact error text is
libglnx's named `openat(O_CREAT|O_EXCL|O_NOFOLLOW|O_NOCTTY|flags, 0600)`
fallback in `open_tmpfile_core()` after O_TMPFILE is unsupported.

The seccomp-export tempfile is a candidate: the pinned Flatpak already calls
`glnx_open_anonymous_tmpfile()` there, honoring TMPDIR. Both locked libglnx
copies honor TMPDIR too. ExtraDataInstallService supplies an app-private
`install/tmp`. The supplied logs contain neither that open's pathname nor a
matching file-create AVC. Consequently /var/tmp mode, SELinux, stale binaries,
and path translation are NOT established root causes. The prior bwrap-only
01777 change cannot explain a failure before bwrap starts.

Denied creates now print logical input, translated backing path (or resolution
failure), dirfd, flags, errno and PID without requiring full trace. Install
stdout/stderr streams to MatonExtraData as it happens. The stub's existing
cache/v3-trace marker now enables loader --trace and Flatpak --verbose for
extra-data too. A coordinator retry must capture these lines; correlate a
relative path with create-dirfd and the preceding trace. Compare the installed
host native-library hash with the newly built loader if no diagnostic appears.

## Fixes and host evidence

* Retained proc directory FDs: stock apply_extra sets NO_PROC, but bwrap still
  opens `self` relative to its original proc_fd after pivot_root to finish the
  emulated user mappings. The proxy rebased this FD into the detached old root,
  then failed with `bwrap: open /proc/self failed`. During bwrap setup, resolve
  relative lookups on native proc FDs against their pinned kernel path. This
  uses the real FD, not stale cached names, and does not expose /proc after
  exec into the payload. No Chrome-specific proxy behavior was added.
* Cleanup: prepareRuntimeSystem makes the staged shared installation read-only.
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
CHROME_TEST_OUT=/mnt/data/aosp/out/pc-logs/chrome-sol/device \
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
