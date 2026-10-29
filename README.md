# MatonOS Linux subsystem app — v3.5

Standalone source repository for the signed `org.matonos.linux` host APK,
generated stubs, OSTree packs, the compatibility proxy/loaders, Flatpak and
embedded PipeWire. MatonOS is based on AOSP. The device tree imports a signed
prebuilt; it does not compile this repository.

## Build and tests

Install JDK 21+, Gradle 9.4.1, SDK platform 36/build-tools 36.0.0 and NDK
30.0.16248370 (r30). Native dependency builds also require a C/C++ host toolchain,
Meson, Ninja, CMake, pkg-config, autotools, Python 3.12+, curl, tar, unzip,
OpenSSL, and the development packages for GLib and OSTree. The tests execute
host `ostree`, compile C/Java fixtures and use the SDK's `aapt2`. No AOSP or
device-tree checkout is required: build-only platform headers, their revision
pins and licences are in `third_party/platform-headers`. AIDL APIs are
app-owned; no system bridge compile snapshot is needed.

```sh
export JAVA_HOME=/path/to/jdk-21
export MATON_ANDROID_SDK=/path/to/android-sdk
export MATON_NDK=/path/to/android-ndk-r30
export MATON_GRADLE_HOME=/path/to/gradle-9.4.1
export MATON_SUBSYSTEM_OUT=/absolute/path/out/linux-subsystem-app
export MATON_SIGNING_STORE_FILE=/private/path/subsystem.jks
export MATON_SIGNING_PASSWORD_FILE=/private/path/subsystem.pass
export MATON_SIGNING_KEY_ALIAS=matonos-dev
./build.sh --build-dependencies  # prepare locked dependencies, test, build, sign
./build.sh                       # reuse prepared dependencies; test, build, sign
```

The script requires an existing external keystore; it never creates or stores
keys in this public repository. It rejects signing inputs and output directories
inside the repository. The password file contains the keystore/key password.
Do not use shell tracing with signing environment variables. Maximum parallelism
is four (`MATON_BUILD_JOBS=1..4`). The result is
`$MATON_SUBSYSTEM_OUT/MatonLinuxSubsystem.apk`; JNI/dex entries are stored,
16 KiB aligned and signed again after ZIP normalization. Updates retain the
same signer. The script verifies the final signature and alignment.

### Image bootstrap

The image ships `MatonLinuxSubsystemBootstrap.apk` as the system
`org.matonos.linux` package at versionCode 1. It declares the same shared
library and package queries as the real host, and embeds the real same-signer
APK as a stored asset. On first boot, or when the bootstrap becomes active
again after uninstalling updates, its boot receiver (or launcher entry)
installs that asset over itself through PackageInstaller. The real APK's
versionCode comes from the repository commit count plus an offset and must
remain greater than 1; subsequent updates use the normal F-Droid path.
StubLoader resolves the installed host library from PackageManager, so
generated stubs become usable after the bootstrap finishes.

Flatpak's source/dependency lock is `linux/flatpak/build-support/sources.lock`:
MatonOS-dev/flatpak branch `matonos/v26.10`, commit
`8078df1477d6b6f1c15efaf327889a52a8adfedc`. Its CLI/multicall recipe was
carried from MatonOS_apexs; image-only store helpers are excluded. The build
keeps source archives and dependency output outside this repository. An existing
verified dependency cache can be selected with `MATON_FLATPAK_CACHE`,
`MATON_FLATPAK_BUILD`, `MATON_COMPOSITOR_BUILD` and `MATON_PIPEWIRE_BUILD`.
The source pin is checked before the APK build. `UPSTREAMS` and the native
recipes retain the other version/hash pins; this extraction does not upgrade them.

`tests/run.sh OUTPUT` runs signed carrier/stub/planner tests, pack C/Java
round trips, host commands, Java portals and native bus interoperability.
`linux/compat-proxy/tests/run.sh` is the Linux-host loader regression suite;
run it in a disposable copy next to `packs/` with a writable `build/` directory,
all outside the source tree. Android instrumentation tests require a device.
`python3 tests/scan-history.py` checks all reachable history, including deleted
blobs, for signing/private-key filenames and private-key/JKS contents.

The host APK build also compiles the offscreen Vulkan forwarder and packages its
ICD library and manifest. It needs the pinned Vulkan-Headers/Loader/Tools sources;
`build.sh` fetches them into the external output when absent. Set `CMAKE` to the
repo's pinned CMake binary (the MatonOS integration build uses
`/mnt/data/aosp/prebuilts/cmake/linux-x86/bin/cmake`). The production x86_64
loader is Android-linked for bionic thread adoption and accepts `--gpu-bridge`
only on the v3 Flatpak launch path. `VK_DRIVER_FILES` points apps at this ICD,
which forwards offscreen Vulkan calls to the Android loader; Mesa app ICD
fallback is disabled in this mode. i386 and host test loaders stay freestanding.

## Split boundaries and provenance

History was extracted using `git filter-repo` on a fresh, non-local clone of
the device repository's `ostree-packs` branch at `28ecbe6`. Original authors,
commit messages and relevant file history remain; commit IDs were rewritten.
No remote repository was created and nothing was pushed.

The native `linux/dbus-broker` is **still used on this source branch**:
`SessionBus` calls `runtime.NativeBroker`, which loads
`libmatonos-dbus-broker.so`. Java implements desktop portals and their wire
codec; it has not replaced that session bus. Removing the JNI broker would
break per-app bus startup, so it and its tests are retained. It now links the
same pinned Flatpak GLib dependency build, eliminating its former separate
AOSP libffi/PCRE build. PipeWire is also live (`NativeAudio` and
`libmaton_pipewire.so`); its embedded library and notices remain here.

Xwayland, xkbcomp, X11 libraries, GL forwarders and keyboard data are packaged
in the APK. `linux/subsystem/build-xwayland.sh` and `UPSTREAMS` own the pinned
build recipe/provenance; `tools/package-xwayland.py` packages that output.
Executables are `libmatonos-Xwayland*.so` and `libmatonos-xkbcomp.so` native
libraries. Keyboard data is extracted from assets into each stub's private
runtime directory, keyed by host update time. Private symlinks retain the
library SONAMEs and the stock xkbcomp name. A small C entry point passes the
keyboard directory to the server and sets its working directory for xkbcomp.
No executable is extracted to writable app data. The image extracts native
libraries from the same verified APK into its app-local nativeLibraryDir;
PackageManager extracts installed updates. Global image Xwayland files are
no longer used. The server is built with `-Dxkb_bin_dir=.`; a stale dependency
build must be refreshed before packaging. Select the inherited Bionic crypto
prefix with `MATON_XWAYLAND_CRYPTO_PREFIX`; its pins and retained-version
exception are in `UPSTREAMS` (the static musl Flatpak prefix cannot replace it).

Device checks after a fresh image boot: confirm the subsystem uses `priv_app`,
execute its own APK native helpers (Flatpak and both Xwayland entry points),
install/download using network and PackageInstaller, then launch an X11 app,
type several keyboard layouts and close/reopen it. Verify Xwayland and xkbcomp
come from the host nativeLibraryDir, no `/system_ext/bin/Xwayland` exists and
no SELinux denials occur. Install a same-signer host update and relaunch an
existing stub to check that its binaries and keyboard assets follow the update.

## Validation and limits

The v3.5 split was built with SDK/NDK paths and prepared dependency caches,
without using device-tree sources. The carried Flatpak recipe rebuilt the
pinned fork against cached dependency builds. Signed APK verification, 16 KiB
alignment, the host suites and device preflight were checked; see the device
integration report for exact results and hashes. A completely empty-cache
bootstrap, AOSP image build, fresh VM boot and real hardware runtime tests
are separate integration checks, not claimed by the extraction.

For runtime verification, build a fresh image containing the pinned APK, boot
it (disable idle suspend in a test VM), install/update a signed OSTree carrier,
launch Wayland and X11 apps, check audio and close/reopen behaviour, then install
a same-signer host update and relaunch a stub. Repeat on Ryzen/RX 6600,
Surface Pro 3 and HP ProDesk; inspect SELinux denials and retain the no-hardware
fallbacks. No kernel or policy changes are required by this repository split.

Licences are preserved at the root and under the individual components,
including `linux/flatpak/licenses`, build-support licences, apksig and platform
header notices. No signing keys belong here.

Live install, pack-artifact, stub-generation, deployment and extra-data work
uses canonical `Context.getNoBackupFilesDir()/work`, never app cache. The
`.v3-store-root` markers remain on staging, launch deployment and applied-extra
roots. No old cache contents are migrated. The store sweeps stale work when its
service starts and on the existing boot cleanup action; stub launch and extra
service startup also sweep their own private work roots. Process-local,
reference-counted leases protect exact live trees; sweeping descends shared
category directories and removes stale siblings without following symlinks.
Leases end at terminal operation cleanup, or process exit for launch mappings.
There are no age heuristics or storage caps.

The Linux application's XDG cache and temporary/cache environment paths stay
purgeable. Its loader-mapped resolver file and extracted pack-launch metadata
live in `work/launch-maps`; deployment metadata lives in `work/flatpak-launch`.
AF_UNIX endpoints use a short path through a held directory descriptor:
`/proc/self/fd/<dirfd>/<name>`. The descriptor stays open for the listener or
client lifetime, so the kernel resolves the endpoint inside the original
private app directory while `sun_path` remains below its 108-byte limit.
Only the final component is length-checked. No names are truncated or hashed,
and no socket directories are created outside app-private storage.

Socket site audit:

| Site | Handling |
| --- | --- |
| DNS helper | The compat proxy lazily starts a per-process UDP helper and queries Android through `android_res_nsend`; there is no host-app listener. |
| DNS payload | The compat proxy rewrites configured UDP DNS requests to the per-process helper's loopback port and resolves them through Android's resolver API. |
| Wayland compositor socket (`compositor_core.c`) | The compositor holds the runtime directory fd and sets `XDG_RUNTIME_DIR` to its short proc-fd path before wlroots binds. |
| Per-session compositor socket (`nativeAddSession`) | The compositor opens and retains the parent directory fd, then binds through the short path. |
| Xwayland socket directory/client | The compositor uses a held directory fd for its Xwayland connection; the child inherits the directory fd while using the same short directory path. Linux payload clients still name the real private runtime path and the compat proxy resolves it on its own side. |
| PipeWire and Pulse sockets | PipeWire holds the runtime directory fd and uses its short proc-fd directory for XDG, PipeWire and Pulse runtime paths. |
| Session bus | The in-process native broker opens and retains the parent directory fd, then binds the bus through a short path. |
| Java portal backend | The Java listener binds through a held directory handle; native broker independently opens and retains the parent fd for its connection. |
| Host-command control socket | Java clients connect through a held directory handle. The compat-proxy listener opens its own parent fd and binds through its own short path. |
| Other AF_UNIX uses | The remaining test-only LocalSockets use abstract names or socketpairs; no additional filesystem bind/connect path sites were found. |

The staging-completion callback previously caught post-download exceptions
without logging them. Terminal result processing marked an operation completed
before unprotected cleanup, allowing an unchecked cleanup failure to prevent
callback delivery and making a subsequent failure report a no-op. Completion
now logs failures (including unchecked failures), checks that required operation
trees remain readable, and delivers the store callback in a cleanup `finally`.
This source audit identifies missing diagnostics and a callback-loss path; the
historical VM incident's exact exception cannot be proven without its stack
trace. No VM was touched for this change.

`tests/run.sh` includes a host bind/connect regression in an absolute
directory over 200 bytes, plus WorkDirs checks for canonical private storage,
legal long package paths, keeping live work, deleting stale siblings/released
work, missing-tree errors, and symlink containment. An Android instrumentation
test binds and connects through independently opened directory descriptors.
Runtime integration checks on a fresh image: install Firefox under
storage pressure and clear app caches while packs are built; confirm work stays
under `no_backup/work` and the operation reports success or an explicit failure.
Interrupt an install, restart the service or reboot, and confirm stale work is
removed; trigger boot cleanup during an install and confirm its live staging and
artifacts survive. Launch a stub, clear its cache, and verify resolver/pack maps
remain available. These checks still require a device; no kernel or shared
image changes are needed.

Steam pressure-vessel follow-up (2026-10-28, round 44): bwrap bind
destinations shadow the immutable EROFS symlink they are bound over. The same
`/usr` image is bound at both `/usr` and `/run/parent/usr` in the staging root;
reversing the physical image pathname used to create the temporary i386
directory under whichever alias `erofs_reverse` happened to pick, leaving
bwrap's actual `O_PATH|O_NOFOLLOW` destination a symlink. Destination creation
now shadows every spelling that reaches the same backing object: the caller's
dirfd view, the erofs prefix alias, every sibling VFS mount whose source is the
image (so `/newroot/usr` beside `/newroot/run/parent/usr`), and the old-root
backing alias. This also covers directory descriptors that arrive by
`SCM_RIGHTS` (the receiver's proxy never tracked a logical name for them and
falls back to the physical fd text). The regression test now exercises both
the tracked-dirfd and passed-fd shapes, verifies the sibling alias is a
directory too, and keeps stock bwrap's refusal of genuine symlink targets.

The mixed-ABI EGL JSON now names the absolute installed i386 library under
`/usr/lib/x86_64-linux-gnu/GL/maton/lib/i386-linux-gnu/`. pressure-vessel treats
relative names containing a slash as relative to the JSON, including after
Flatpak merge-dirs relocation; the previous value pointed into egl_vendor.d.
GL32's separate extension still uses its own library basename. The layout test
checks that the absolute mixed-ABI name resolves to an actual packaged file.

Setup profiling from day-r49: pressure-vessel pid 22892 made 43,385 successful
openat calls, 9,617 unlinkat calls and 6,597 linkat calls during 19:58:24 to
20:00:30. Mount lookup now caches immutable target lengths and skips shorter
candidates before comparison; the 512-mount, 10,000-open directory-relative
scan measured 51.3 us/iteration before and 39-49 us after on this host (about
24% lower under load). The traced capture contains 1,128,773 zero-only
raw-counter lines: counter-disabled builds no longer emit these. Per-second
line volume stays near a constant ~8-16k for the whole window (a logd-saturated
throughput, not per-phase work), and the busiest phases (1,400-3,100 path
syscalls/s) only appear late, so the ~2 minutes are dominated by trace logging
rather than path translation. The tracer now emits the per-trap alt-stack line
only when the stack placement changes: it cost ~17 log writes per handled
syscall (~5.4M writes for ~317k traps) for bytes that only change when the
alt stack is installed. Combined with zero-count suppression, trace-mode log
writes drop by roughly half, keeping captures usable for debugging.

Resolve-cache invalidation is now lazy: entries carry the epoch at which they
were published and invalidation bumps a counter instead of clearing the table.
bwrap's setup interleaves directory scans (which populate the cache) with
mkdir/bind writes (which used to empty it on every write, turning each later
scan back into full per-component resolution). Stale entries are skipped by
the epoch check, dropped by a compaction that runs only when the table would
otherwise grow, and never expose a freed mount: unmounts bump the epoch before
freeing, so a live entry can only reference a mount from its own epoch. The
first lazy attempt had a real defect (the free-slot index was captured before
compaction and could land past the array after it reclaimed entries); it is
rebuilt with the slot read after reserve and verified against host Chrome
headless, the resolve-cache test and the full proxy suite.

Validation for this change is host-only, with no VM started. The EROFS alias
regressions (including SCM_RIGHTS dirfds), GL-extension layout tests, the VFS
resolve-cache test and the proxy suite pass. Rebuild the loaders and GL
extensions, then verify Kate/Obsidian/Chrome still draw and Steam passes
`/run/parent/usr/lib/i386-linux-gnu` setup without the EGL resolved-path
warning. Check pressure-vessel setup duration and webhelper initialization; no
kernel or shared device-tree changes are needed.

### Steam round 45: pressure-vessel setup and CEF adoption

In day-r50, pressure-vessel PID 26366 starts at 21:08:51.028 and execs its
container/launch-client at 21:10:28.422: **97.394 seconds**. Its setup window
contains 42,627 openat, 44,691 close, 6,599 linkat, 7,867 fchmod and 6,719
utimensat results (counts exclude interleaved/corrupt records). The trace has
no per-call elapsed times, so these are operation counts, not a CPU profile;
r44's logd-saturation caveat still applies. Do not claim an untraced setup time
or a sub-ten-second device result from this capture.

Ordinary file creation/link/unlink invalidated the marker-ancestry cache on
every sibling, repeating the same root walk throughout runtime copying.
Those operations cannot change directory ancestry, and mutation of marker
names is already rejected. Keep invalidation for directory removal/creation,
rename, symlink and directory metadata operations. The host regression copies,
links, stats and removes 128 siblings under a marked root, counting raw marker
opens with strace: main makes **3,588** probes; the fix makes **11**. It checks
real link counts too. Full compatibility and recovery tests remain required;
this count reduction is not a prediction of device wall time.

The last wrapper's real CEF binary is PID 26508 (`./steamwebhelper`), entered
at 21:10:29.838. GPU children 26531 and 26577 fail resident adoption/EGL, then
exit_group(0) at 21:10:32.210 and 21:10:34.176 respectively. The browser
process remains alive receiving/polling until the capture ends at 21:12:34;
there is no browser exit to explain in this run. Its GL path fails first;
the Vulkan surface-extension errors are a later fallback, not evidence that
adding an extension name will fix initialization.

The adoption trace exposes a separate identity bug: unregistered resident
workers call `adopt_current`, whose singleton fallback incorrectly treats
any new TID as a fork child. Day-r50 shows `adopt-recovered-fork-child` for
TIDs 26118/26119/26122 **in PID 26110**; they steal/rebase the sole payload
record. Later CEF children report `adopt-miss-map 0 0`, and EGL cannot find
an adopted Android thread. Require a changed kernel PID before singleton
fork recovery. Normal TID/FS lookup is unchanged; genuine fork rebasing
updates the registry's process identity. No threads are retired and signal
routing/masks are unchanged.

The added native-worker regression fails on main (exit 53, worker steals the
record) and passes with the fix, retaining the main record and native FS.
Full compat-proxy suite, real Flatpak/bwrap + resident workers, Wayland
roundtrip, signalfd, production Android loader builds and the real-bionic
ELF32-to-ELF64 GPU fixture pass. The GPU fixture covers fork and resident
exec with a synthetic native Vulkan provider; it is not device rendering.
Evidence: `/mnt/data/aosp/out/pc-logs/agents/steam-r45-suite.log` and
`/mnt/data/aosp/out/steam-r45/{gpu64.log,marker-negative.log,preflight.log}`.

Coordinator follow-up: rebuild all loader peers, fresh-boot, verify
Kate/Obsidian/Chrome still draw, then capture traced Steam and current-run
webhelper-linux/steamui_html logs. Confirm same-process singleton recovery
and `adopt-miss-map 0 0` disappear, EGL initializes in CEF's GPU child, and
measure pressure-vessel setup again. A successful Steam window and <10 s
setup remain unverified. No VM, AOSP, kernel or shared image changes.
