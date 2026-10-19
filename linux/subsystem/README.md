# Per-app compositor host

Each signed stub runs PerAppRuntime, the wlroots native core, Xwayland,
SessionBus, JavaPortal, SessionAudio and DNS forwarding in its own process and
UID. The host APK supplies the shared library and native assets; it does not
coordinate another app's Linux launch. V3Launcher runs the compat-proxy loader
from the host nativeLibraryDir. See [v3 launch](V3-LAUNCH.md),
[stub format](STUBS.md) and [DNS](DNS-FORWARDER.md).

StubService creates `<dataDir>/runtime` itself, canonicalizes the path and
sets mode 0700. Wayland, bus, portal-backend, pulse and X11 sockets stay below
that PM-owned app data directory. No privileged runtime preparation, shared
socket namespace, registration supervisor or shared compositor remains.

Native sources and pinned dependencies are built outside Soong with
build-compositor.sh/build-xwayland.sh, using scratch/output under out/.
The in-process JNI broker remains because SessionBus loads it; its standalone
entry, privileged session control and unused SysV supervisor are deleted.
Closing the final explicit app window ends the stub runtime; minimizing or
recreating an activity preserves it. Relaunch apps after host updates during
pre-alpha. MatonOS is based on AOSP.

Local javac, native broker and compat-proxy results are recorded in
out/pc-logs/agents/v2-retire-result.md. Fresh-image enforcing validation remains:
verify Wayland and X11 rendering, portals, audio, DNS, close/reopen and updates,
and denial of another UID opening the app's sockets. Repeat on the supported
PC test machines. SysV emulation and broad shared-folder access remain gaps.

### Keys without scan codes (round 16)

WindowActivity and StubActivity forward `KeyEvent.getScanCode()` unchanged
through JNI. Positive scan codes are raw Linux evdev codes and retain their
existing path. wlroots adds the XKB offset internally. When no scan code is
available, `native/android_keymap.c` translates the framework keycode using
an inverse of AOSP `frameworks/base/data/keyboards/Generic.kl`. The first
numeric mapping in file order wins; duplicate choices are recorded in the
generated file. HID usage mappings are excluded because they are not evdev
codes. Unmapped events are dropped before focus or keyboard state changes,
with one debug log per process; no numeric keycode offset fallback remains.

Regenerate from the local AOSP checkout, then review the diff:

```sh
python3 linux/subsystem/native/tools/generate_android_keymap.py /mnt/data/aosp > linux/subsystem/native/android_keymap.c
cc -std=c11 -Wall -Wextra -Werror linux/subsystem/native/tests/android_keymap_test.c linux/subsystem/native/android_keymap.c -o /tmp/android-keymap-test
/tmp/android-keymap-test
```

Validation: standalone host test passed, all 240 mappings were checked against
Generic.kl, and regeneration was byte-identical. `build-compositor.sh` passed
(dependencies), then the native CMake target compiled and linked with NDK r30
in a separate out/ directory. No APK/image build or device test was performed.

After the coordinator builds a fresh image, on each supported PC launch a
Linux text editor and verify physical keyboard letters, modifiers and arrows.
Send scan-less DPAD_DOWN/UP/LEFT/RIGHT, ENTER and DEL events with
`input keyevent`; verify cursor movement, newline and backspace. Check an
unmapped keycode produces no key in the app, and IME-delivered KeyEvents use
the same mapping. IME text commits that do not produce KeyEvents are outside
this change. No shared-file or kernel changes are required.
### Round 17: fullscreen modes and runtime children

Mapped fullscreen X11 configure requests now reach Xwayland unchanged; Android
still owns the presentation size. Windowed windows continue to use Android's
size constraints. Previously `on_xtrack_configure` discarded mapped requests
and `configure_window` forced every resizable drawable to the Android size.
Xwayland's `xwl_window_should_enable_viewport` requires the drawable to match
its per-client emulated RandR mode, so that policy prevented its viewport
scaling. The compositor already exposes wp_viewporter and the presenter uses
scene source/destination geometry. Xwayland server creation now explicitly
selects RandR emulation as well. No game-specific treatment is involved.
The 1280x720 GL viewport symptom is consistent with this mismatch; the actual
ETR race still requires a freshly built image to verify.

wlroots double-forks Xwayland. The runtime becomes a child subreaper before
creating it and pins its executable-matched adopted child with a pidfd. Native
shutdown kills that pinned process and attempts reaping for at most 100 ms.
The last-window grace callback explicitly stops the native runtime before
killProcess, which otherwise skips Service.onDestroy. Grace, minimization,
activity recreation and multiple-window policy remain intact. Lazy creation
still allows a clean server on the next per-app runtime launch. A native stop
also checks for a child that started without reaching the ready callback.

Validation: NDK r30/API 35 compositor library build; all host Java sources
compiled against SDK 36 (one existing deprecated API warning);
`native/tests/fullscreen_mode_test.py`, `geometry_request_test.py`, and the C
`xwayland_lifetime_test.c` passed. The lifetime test executes a real double
fork and verifies adoption, pidfd termination and reaping. No adb or image
build was performed. Existing runtime_review_test.py has a stale 0711 runtime
mode assertion while current maton_core_start deliberately uses 0700.

Coordinator fresh-image checks: launch an SDL2 X11 game at 1600x900, select
1280x720 fullscreen and start gameplay; verify full-window scaling and aligned
pointer coordinates, then restore native mode/windowed mode. Quit normally,
wait the ten-second last-window grace, and check the old Xwayland/stub PIDs
are gone; relaunch and check that a new display/socket works. Repeat closing
one of two windows, minimize/restore and closing during Xwayland startup.
On the Ryzen/RX6600 PC, Surface Pro 3 and ProDesk, repeat the same checks
without changing game-specific settings or host device permissions.

### Flatpak install staging lifetime

Pack creation now commits the deployed `files/` tree as a private ref in that
package's existing bare-user-only Flatpak repo. This avoids creating a second
OSTree repo and duplicating the deployed tree. Once the package's signed pack
artifacts have been written, the installer removes that package's download
installation and any legacy `pack-repo` before starting PackageInstaller.
Other packages retain independent repositories until their own artifacts are
ready. Pull verification, package signatures and failure cleanup are unchanged.

The host check committed a deployed tree with an executable, regular file and
symlink into a bare-user-only repo, then passed `ostree fsck --all`; it did not
change the repo mode. The subsystem APK build and the pack, compat-proxy and
combined tests passed. On the headless VM, the pre-change VulkanInfo attempt
committed the package. During the observed retry series, logcat reported a
2,872,459,264-byte peak operation allocation after packing; the sampled stage
tree reached 1,864,876 KiB and `MemAvailable` reached 3,854,632 KiB. The later
VulkanInfo update with the new APK completed in about four seconds. Its
allocated staging tree fell from 19,791,872 to 9,412,608 bytes after pack
artifacts were written, and the post-update sampling interval had a minimum
`MemAvailable` of 5,527,008 KiB. The VM already had VulkanInfo installed, so
this update run is smaller than a fresh install and is not a like-for-like
peak comparison. Its package remained installed; no reboot or uninstall was
used. The baseline VulkanInfo process later exited after SELinux denied access
to `/tests`; no `LOW_MEMORY` kill occurred during the observed successful
baseline install.

To measure on a fresh install or a real PC, run
`org.matonos.testinstaller/org.matonos.linux.VulkanInfoInstallActivity` on a
freshly booted test image, then sample
`/data/user/0/org.matonos.linux/no_backup/work/flatpak-staging` with `du -sk`
while recording `/proc/meminfo`'s `MemAvailable`. Keep the stub installed
between runs; use an installed-package update for subsequent measurements.
Check logcat for `Temporary operation allocated bytes after packs` and
`Released Flatpak and pack repositories`, and confirm the stage is cleaned
after success or failure. Repeat with a large runtime/app and a constrained
RAM VM to compare like-for-like fresh installs. A full fresh-install
before/after VM comparison remains open because this VM already contained the
VulkanInfo stub.

### Vulkan WSI handshake diagnostics (round 19)

`MatonWSI` is an opt-in, capped (512 messages per compositor lifetime)
server protocol trace. Enable `log.tag.MatonWSI=DEBUG` before opening a fresh
stub process. It records client PID/fd, object/interface, request/event,
argument count and first scalar/object ID; it omits strings and arrays.
Look for xdg configure/ack, `wl_display.sync`/`wl_callback.done`, SHM pool
and buffer creation, surface attach/commit, buffer release and frame done.

Mesa 26.2.3's software Wayland WSI binds wl_shm, not linux-dmabuf/syncobj.
Its first FIFO present starts ready; later FIFO presents wait for a frame
callback. The compositor advertises SHM via the pixman renderer and does
not advertise presentation-time, fifo, commit-timing or tearing-control.
The Rio syscall trace alone does not prove where its first present waits;
no presentation behavior change or Rio success is claimed for round 19.

`native/tests/wsi-host.py --deps <pinned compositor build directory> --out
<scratch under out/> [--proxy <linux-layer-v3>]` compiles a minimal host
SHM/xdg peer and checks eight lavapipe FIFO presentations from the installed
vkcube-wayland. This is a protocol/proxy smoke test, not a production
wlroots rendering test. It uses the host Vulkan ICD, whose version may
differ from the Flatpak runtime. Exit 77 means the sandbox denied
wl_client_create; no presentation test ran. NDK compositor compilation
and linking passed; host Vulkan testing was blocked by that restriction.

Device test: rebuild through the coordinator, boot freshly, disable VM
idle suspend, enable MatonWSI, then launch a fresh Rio stub and collect
MatonWSI/MatonCompositor/MatonWLR/MatonV3. Check configure acknowledgement
and each handshake milestone; then check ETR, close/reopen and resizing.
No shared files, kernel or policy changes are required.
