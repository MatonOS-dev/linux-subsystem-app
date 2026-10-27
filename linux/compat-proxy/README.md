# Linux layer v3: syscall compatibility proxy

The compatibility proxy lets Linux x86_64 and i386 processes run inside their
MatonOS stub app process. Production loaders are Android-linked executables:
the Android app identity, inherited seccomp policy and SELinux domain remain
in effect. The proxy translates Linux ABI calls and emulates selected kernel
objects; it is a compatibility layer, not a security boundary.

The x86_64 and i386 loaders share canonical syscall IDs and core VFS, network,
SysV IPC, exec-state and EROFS-index code. Per-architecture backends own raw
syscalls, BPF filters, register contexts, syscall number maps, user-buffer
layouts, ELF details and thread/GPU adoption. Cross-class exec is handled by a
verified sibling loader and a sealed, versioned state stream.

## Build and host checks

From this directory:

```sh
bash build.sh
bash tests/run.sh
bash tests/i386-p1.sh
```

The host build uses the system C compiler and strict warnings. Android
production loaders are built with `build-android.sh`; the standalone i386
loader is built with `build-i386.sh`. Build outputs stay under `build/` here.
Host tests cover syscall translation, ABI crossings, VFS and links, networking,
DNS, SysV IPC, EROFS package prefix swaps, threads/signals and loader lifecycle. They
do not establish compatibility with Android's installed app policy, graphics
labels or device rendering.

`tests/erofs-mkdir-host.py` reproduces bwrap's mkdir-parents checks through a
virtual pivot with `/usr` mapped to a package image. Existing directories must
return `EEXIST`, new payload directories must return `EROFS`, and bwrap's
missing bind targets get temporary VFS directories that its next virtual bind
can replace. `tests/erofs-single-backing-host.py` models the physical OSTree
deployment `files/` directory passed by the stub: opening the bwrap target and
then using its dirfd must resolve to the associated image branch, even when
the physical directory is empty.

The mkdir regression also covers Compat.i386 when its extension point is a
symlink in the immutable EROFS runtime. During bwrap setup the runtime can be
bound below a staged root while the process still uses an outer root; the
temporary directory must shadow the symlink at that staged alias. The test
checks both the direct-root bind and this staged-root case, then verifies the
extension marker is visible through the bound directory.

`tests/mkdir-home-host.py` checks that a bwrap-bound writable app home can
create new directories through `mkdir` both with and without a trailing slash.

For a single-image deployment, the loader receives `--erofs-mount <target>
<mount>/<branch>` and `--erofs-backing <target> <deployment-files>`. The
second association lets path resolution replace both the virtual package
target and its physical deployment prefix with the same mounted image path.
After that swap, filesystem operations use native kernel EROFS behavior.

Optional diagnostics:

- `--trace` writes trap and virtual-operation details to the saved diagnostic
  descriptor.
- `--count <path>` records handler time, VFS path-resolution time and raw
  syscall totals. Compile with `PROXY_RAW_COUNTER=1` to enable per-trap raw
  syscall counts.
- `--parent-pid <pid>` applies parent-death handling and validates the parent.
- Host-only harness options (`--root`, `--map`, `--map-ro`,
  `--erofs-mount`, `--android-filter`) are used by tests and host development.

## Runtime responsibilities

### Syscall and process compatibility

The handler receives trapped calls through per-ABI seccomp filters. It converts
legacy syscalls to modern equivalents where safe, emulates selected calls, and
returns `ENOSYS` for unsupported operations. The filters preserve the incoming
Android policy and only add the loader's routing rules. i386 and x86_64 syscall
numbers are generated from the checked-out UAPI headers; i386 aliases account
for legacy wire names such as `mmap2`, `fstatat64` and `fcntl64`.

`personality()` is routed before Android-library fast paths, i386 adoption and
Flatpak's emulated payload filter. Queries return the current per-thread
persona (seeded from the kernel); sets use the real syscall when Android
permits it, and otherwise retain the requested persona and return the previous
value when Android reports `EACCES` or `EPERM`. This preserves the API's
query/set state, not kernel side effects:
the resident loader already handles ELF32 exec itself, while kernel-controlled
exec layout, ASLR and mmap permission effects remain unavailable when the
kernel refuses the request.

The resident ELF loader maps static, glibc and bionic ELF images, their
interpreters, argv/env and auxv. Resident exec keeps the proxy in the process,
resets ABI-specific TLS and closes payload CLOEXEC descriptors. Cross-class
exec validates the trusted sibling executable and transfers pointer-free
runtime state through a sealed memfd. Signals are forwarded/chained with the
payload ABI context; clone/fork, thread exit, robust lists, and selected
pthread TLS behavior are adapted by architecture backends.

The Android launch path uses the loader's `--supervise` mode. A small
child-subreaper process owns the ordinary Flatpak/bwrap loader and waits for
orphaned Linux descendants before returning the launch status to Android.
This keeps the stub process and app task alive when a launcher exits after
forking its real application process.

With `--gpu-bridge`, the loader publishes the GPU bridge entry points and
adopts resident bionic pthreads while GPU calls enter Android graphics
libraries. Adoption is opt-in and has separate x86_64 and i386 implementations.

### Virtual filesystem and links

`vfs.c` keeps a process-local mount/cwd/namespace view and translates payload
paths to backing paths. It resolves dirfd-relative paths and symlinks, tracks
fd aliases, and implements selected mount, pivot, chroot, unshare and proc
views needed by bwrap. This state is inherited across fork and transferred on
cross-class exec. The host-only `--root`/`--map` scaffolding is not a production
security mechanism.

The proxy initializes `/dev/shm` as a unique directory under the app-private
`TMPDIR` before launching a payload, so Steam Runtime and nested bwrap can use
it even when the Android host does not provide that path. bwrap's own `--dev`
then creates its normal private `/dev/shm` inside each sandbox; the proxy treats
the standard `mkdir /dev/shm` setup as successful when the mapped directory is
already present. Stale backing directories are removed on a later launch.
POSIX `shm_open()` files and `MAP_SHARED` mappings work between processes in
one app session. This storage is session-private and distinct from
`--share=ipc`, which controls the IPC namespace. `tests/posix-shm.c` checks
exclusive creation, unlink, and cross-process shared mappings through bwrap
and the proxy.

`links.c` provides indexed hardlink behavior for marked OSTree-style stores.
`erofs-index.c` keeps only package-target-to-mounted-directory prefix swaps.
After the swap, `stat`, `readlink`, `getdents`, `open`, `mmap`, and descriptor
operations use native syscalls against the kernel EROFS mount. It keeps no
deployment file index, layer table, tracked EROFS fds, or synthesized EROFS
metadata. The generic fd reserve protects proxy-owned descriptors with an
exact 18-slot reserve at the top of the soft limit, close-skip and move-on-dup
guards, and no payload data-call fd checks. SysV IPC metadata and segments
share one private memfd arena.

### Network and DNS

`net.c` emulates the small virtual `NEWNET` surface currently needed by bwrap:
loopback interface queries and a constrained route-netlink response queue.
Requests outside that surface fail explicitly; calls outside virtual `NEWNET`
continue through the kernel.

`dns-netd.h` rewrites only UDP requests addressed to the configured loopback
DNS endpoint. A lazy per-process helper submits DNS packets to Android's
`/dev/socket/dnsproxyd` using netd's `resnsend` protocol and returns responses
to the app's original UDP sockets. The proxy preserves app-visible socket
numbers and DNS peer addresses. DNS-over-TCP is unsupported. Ordinary network
sockets retain kernel behavior. The helper restarts lazily after fork.

### SysV IPC and state

`sysvipc.c` implements SysV semaphores, shared memory and message queues using
one shared memfd arena, offsets and futex coordination because the Android
kernel is built without SysV IPC. Segment pages and semaphore/message metadata
share the arena; growing its memfd leaves existing shared mappings valid. The
implementation follows Linux ABI layouts and supports
selected lifecycle, blocking, timeout and `SEM_UNDO` behavior; unsupported
operations are reported by the tests and source comments.

`state.c` serializes process state for mixed-ABI exec. The stream uses fixed
width words, bounded lengths, validation, a random handoff token and kernel
memfd seals. `net.c`, `vfs.c`, `sysvipc.c` and proxy-owned state each serialize
their own records through this common interface.

The proxy reserves 18 descriptor numbers at the top of the real soft
`RLIMIT_NOFILE` at startup. They stay occupied by proxy-owned descriptors or
placeholders; the limit is never raised. Current users include the state
handoff memfd, diagnostics, GPU bridge and DNS helper channels, the SysV IPC
arena, and VFS temporary descriptors. If the app lowers its soft limit, the
proxy moves reserved descriptors below it before applying the requested
limit. BPF checks trap only closes/dup targets in the reserved set and
`close_range` sweeps that overlap it. Sweeps preserve reserved descriptors;
direct close of one returns `EBADF`, and dup onto one first moves the proxy
descriptor and then performs the app operation. The old fd-window tiers and
general data-call fd checks are gone. EROFS descriptors use the kernel's
native behavior after the package prefix swap.

When a dup target lands in the reserve and every other reserve slot is
occupied, the displaced proxy descriptor spills to the first free ordinary
fd and stays in the private-fd table. `F_DUPFD` and `F_DUPFD_CLOEXEC` use the
requested minimum as a target only when it is private; the app still receives
the exact kernel-selected result. This keeps high fds below the reported soft
limit usable by Flatpak launchers such as zypak.

## Current limitations and device validation

- Host checks do not replace a fresh-image boot or app-UID testing.
- Fixed-fd verification passes the close-range/setrlimit test, placeholder
guards, host-command-pair integration, and the captured FUSE EROFS ldconfig
replay. No fresh image or VM validation was performed for this merge.
- The enforcing v46 VM fixes v45's 32-bit graphics labels and vendor
  `libhardware.so` dependency. Under the installed Flatpak stub UID, ELF32
  EGL pbuffer and GLX core-profile pixel readback, GLX alpha checks and Vulkan
  physical-device enumeration pass. x86_64 rendering and Tux Racer remain
  regression controls. This establishes the tested virgl path; native PC GPU
  drivers still need their own hardware checks.
- An ELF32 app still requires its Compat.i386/GL32 runtime and the per-app
  Flatpak multiarch feature. The tests use a disposable override; production
  does not grant this feature globally.
- Virtual NEWNET covers the current loopback/bwrap setup only; it is not a
  general network namespace implementation.
- The DNS forwarder supports UDP only and depends on Android netd's
  `dnsproxyd` socket and wire protocol.
- i386 LDT emulation supports 32-bit data descriptors and flat 32-bit,
  nonconforming code descriptors. Far call/jump, `retf` and `iret` control
  transfers through those code selectors are emulated with the real user CS
  (`0x23`); reading `%cs` therefore reports `0x23`, including while executing
  code reached through an emulated selector. Code descriptor limits are
  deliberately ignored, which is laxer than hardware. Nonzero-base code
  (which needs binary translation), conforming code and all 16-bit code remain
  unsupported and are rejected with `EINVAL`.


For production acceptance, freshly boot an image, disable idle suspend in the
VM, run the installed host and stubs under their app UIDs, then validate
x86_64 and i386 process startup, Flatpak launch, graphics/Vulkan, DNS, and
lifecycle behavior. Do not use remount or hot-patching as a pass.

## Changelog

- **2026-10-27:** Fatal-signal reports now include the active Linux
  executable path. Android `/proc/self/cmdline` remains the resident loader's
  argv across emulated Linux execs, so it cannot identify an aborting payload;
  the proxy records the path as it maps each exec image instead. The
  `exec-signals-host.py` regression checks that a post-exec crash names the
  crashing fixture.

- **Steam round 6 investigation:** the available day-r9 trace records Linux
  PID 14344 receiving SIGABRT from `tgkill` (`si_code=-6`) at glibc
  `libc.so.6+0x75f3c`. The trace does not contain an assertion message or a
  stack, so the abort trigger is still unknown. `DBUS_FATAL_WARNINGS=0` is
  present in bwrap's environment and does not establish a fatal D-Bus warning.
  The fatal report now identifies the loaded executable on a repeat run. The
  black Steam window is also unresolved: capture a fresh-image launch with
  v3-trace enabled, the full logcat/proxy stderr, and the Steam per-user
  `~/.var/app/com.valvesoftware.Steam/logs/` files; include a screenshot while
  the window is black. Use the reported executable/PID to capture its live
  `/proc/<pid>/cmdline` and status before it exits. No VM was used for this
  diagnostic change, and no generic runtime fix is established yet.

- **2026-10-27:** Allow Linux `dup2`/`dup3` to replace a descriptor previously
  owned by a resident Android library. After a successful replacement, update
  ownership from the source descriptor; an ordinary source releases the old
  resident ownership record. This fixes adopted children whose Flatpak/zypak
  setup duplicates onto a low fd held by a resident library. The resident-fd
  regression now forks under the proxy, replaces that descriptor in the child,
  and checks the parent's descriptor remains intact.
- **2026-10-24:** Investigated 32-bit LDT code segments for Steam. The proxy's
  raw `modify_ldt` attempt is trapped again by the inherited Android app
  filter; a focused `strace` shows the blocked syscall and recursive SIGSYS.
  `set_thread_area` rejects a 32-bit code descriptor with `EINVAL`. Since the
  user policy is zero AOSP patches, far `jmp`/`call`/`retf` and CS loads are
  dropped for now. The i386 fixture asserts explicit rejection. Steam's test
  installer separately stops before download because dependency planning asks
  Flathub for the unavailable `runtime/org.freedesktop.Platform/i386/26.08`.
- **2026-10-24:** Emulated flat 32-bit LDT code descriptors and far control
  transfers in the compat proxy. Code limits are ignored; `%cs` remains the
  real `0x23` selector. Non-flat/conforming/16-bit code remains unsupported.

- **2026-10-24:** Fixed i386 resident TLS discovery under Android's app policy:
  `get_thread_area` (244) is blocked even from a raw gateway; querying an
  untracked initial descriptor returned `-ESRCH` and caused loader exit 5.
  Native bionic startup now reads its x86 TLS self pointer; payload queries
  use tracked descriptors. The device adoption test enables the app policy.
  The corresponding GLX frame-pacing fix uses i386 timing syscalls.
- **2026-10-24:** Added i386 data-segment emulation and resident thread/GPU
  adoption; recorded the original v45 image-side graphics blockers.
- **2026-10-23:** Added virtual loopback interface ioctl support in NEWNET.
- **2026-10-21:** Preserved resident graphics descriptors and resident pthread
  alternate-stack startup; added fd-lifetime coverage.
- **2026-10-19:** Added packed deployment file support and simplified
  hardlink-store ownership/indexing.
- **2026-10-26:** Replaced EROFS layer-index lookup with one mounted image per
  package and target-to-mount prefix swaps. EROFS path metadata and descriptors
  now stay entirely in the kernel.
- **2026-10-26:** Preserve package-prefix swaps through virtual roots and
  pivots. Existing directories on read-only package images return `EEXIST` to
  bwrap's mkdir-parents checks; new payload directories return `EROFS`, while
  missing bwrap bind targets are represented by temporary VFS directories.
- **2026-10-17:** Added indexed hardlink emulation and killed-operation
  recovery coverage.
- **2026-10:** Replaced the separate DNS forwarder with an in-proxy helper
  that talks directly to netd's `dnsproxyd` socket.
- **2026-10:** Added resident exec, mixed-ABI loader handoff, virtual VFS,
  network compatibility, SysV IPC, HostCommand and GPU bridge support.

Run the device adoption regression independently of graphics with
`bash tests/i386-adopt-device.sh /absolute/external/output 127.0.0.1:5556`.
It exercises main plus six glibc pthreads, nested scopes, all three TLS slots,
FP state, robust owner death, fork and collision rejection under the generated
Android app syscall policy. The root harness supplements, rather than
replaces, rendering tests launched through an installed app stub.

For native hardware validation, use an image containing both vendor graphics
ABIs, install Compat.i386 through the subsystem, and launch the four probes
through a multilib Flatpak stub with its per-app multiarch feature. Require
EGL/GLX pixel readback, matching alpha pixels and Vulkan's actual GPU name;
then repeat the x86_64 probes and Tux Racer. Repeat on the RX 6600, Surface
Pro 3 Intel GPU and HP ProDesk Haswell GPU; the VM result does not establish
those native-driver paths. Keep SELinux enforcing and disable VM idle sleep.

### Adopted fork altstack regression (2026-10-27, round 4)

Host isolation identifies `d2de7fa`, rather than Chrome's `9c55e20`, as
introducing a nested SIGSYS in altstack replacement. The new helper issued
`sigaltstack` at an unapproved `.text` PC. The real proxy BPF traps that PC;
inside the non-reentrant adoption SIGSYS action, Linux kills the process
because SIGSYS is already blocked. The earlier unfiltered assembly test
could not detect this. A filtered probe built against `091429f` returns
EPERM at the old install, `d2de7fa` dies with signal 31, and the parallel
Chrome commit `9c55e20` retains the old EPERM behavior. The latter two
baseline probes supply a raw-call reference because they predate the helper.
This isolates the helper failure; the exact device main-process death still
needs a fresh-image confirmation (no VM was run in this round).

The helper now calls the existing approved `proxy_raw` gateway from its
scratch stack. Rebase first tries the ordinary raw registration: clone retry
already executes outside the signal stack and must not switch onto that
same registered mapping. An active signal frame needs a fresh mapping and
an off-stack registration; detect the live stack using the kernel query as
well as the saved adoption mapping, since proxy thread state can register a
different stack. Keep the active ucontext's stack registration synchronized
for `rt_sigreturn`. No app-specific behavior or seccomp exemptions are added.

`tests/run.sh` runs the unfiltered assembly reference, a freestanding probe
with both production filters, and the real resident rebase implementation
inside SIGSYS in the main process and a fork child. Both filtered probes
perform mprotect after returning from the reinstallation; the resident
probe also exercises registration from a clone-retry-style continuation
outside the stack. The existing Flatpak-filter fork/clone3 fallback/clone/
exec fixture and stock staged musl bwrap-to-exec tests pass. The full suite,
including the previously failing apply-extra fixture, passes on this host.
Logs are in `/mnt/data/aosp/out/pc-logs/fork-adopt-r4/` (machine local).

Trace numbers after `ours` are internal operation IDs: 135 is ioctl,
181 mmap, 187 mprotect (not native syscall numbers). ioctl -25 is ENOTTY,
-13 is EACCES; these lines alone do not establish the fatal cause.
`lifecycle-clone 4113` is 0x1011 = CLONE_PIDFD | SIGCHLD, after the proxy
strips CLONE_VM | CLONE_VFORK from the original spawn request.

Device follow-up: rebuild the subsystem APK with these loader sources and
fresh-boot the coordinator's image. Enable v3-trace and launch stock Kate,
Obsidian and Steam separately. Check fork children through exec, absence of
`adopt-fork-altstack-install` errors, and supervisor raw status 31/34048.
Kate should get past the clone3 fallback and PIDFD clone; Obsidian's zypak
children should get past signal setup; Steam's bwrap ldconfig should finish.
Collect the complete trace including main-process startup and raw-call tail.
No shared device-tree changes or kernel changes are needed.

Round-5 fork adoption: DNS fork preparation runs inside SIGSYS and saves a
kernel mask containing the handler's implicit SIGSYS block. The assembly
clone retry completes after sigreturn; restoring that mask there blocked
SIGSYS in both parent and child. Their next trapped syscall killed them
without entering a handler. `dns_fork_result` now removes that bit before
restoring the mask. This is independent of altstack adoption: traced clone
flags 4113 are 0x1011 (`CLONE_PIDFD|SIGCHLD`), after the proxy has removed
`CLONE_VM|CLONE_VFORK`, so these continuations have private memory.

`tests/adopt-fork-launch.c` runs the real resident handler and proxy filters
with `flatpak-seccomp-fork`: fork, clone3 ENOSYS, vfork-style clone retry,
inherited fd 15 duplicated onto 4, signal setup, exec, and libc posix_spawn.
The old DNS mask restoration reproduces child and parent SIGSYS on the host;
the corrected version passes. The payload altstack query is virtual and
must not be confused with the resident kernel stack; the separate rebase
fixture checks the actual resident stack.

Obsidian's round-5 `android 35 -9` means common syscall ID 35 (`dup2`), not
native syscall 35 (`nanosleep`). Adoption does not itself close fd 15, and
the inherited-descriptor regression passes. That trace cannot distinguish a
missing source from a proxy-owned target. Trace-mode EBADF now logs source
kernel F_GETFD, source VFS-private state, and target private/ownership state.
Re-run stock Kate and Obsidian with v3-trace on the coordinator's rebuilt
subsystem. Kate should pass the retry into exec without raw status 31;
for Obsidian collect `dup-source-kernel`, `dup-source-private`, and
`dup-target-private` if exit 127 remains. No per-app workaround, shared
image configuration, kernel change, VM run or push is part of this fix.

Host validation for this change: the complete `tests/run.sh` stops at the
zygote-startup credential fixture (`bytes=13`, correct child PID, flags=0).

Round-8 fd reserve regression: the Obsidian trace reported `RLIMIT_NOFILE` of
32768 and zypak failed to `dup(32750)`. Zypak's stock spawn path enumerates
`/proc/self/fd`, then duplicates every listed descriptor. The proxy's fd
reserve occupied that number, and its intentional `EBADF` for duplicating a
private fd exposed that the proc listing included proxy-owned entries. The
proxy now omits private descriptors from `/proc/self/fd` and
`/proc/thread-self/fd`. The host fixture runs with the same soft limit in the
adopted clone-retry child, checks zypak's enumerate-then-dup sequence, then
allocates fd 32750 with `F_DUPFD_CLOEXEC`, moves it to fd 4 and verifies it
survives exec. If no spare reserve slot remains, `proxy_private_fd_move`
relocates the displaced private fd below the reserve instead of rejecting the
app's request. The full `tests/run.sh` passes on the host. Re-run Kate and
Obsidian on a fresh coordinator image with v3-trace to verify the device path;
no VM was used for this change.
A separate loader built with the unchanged DNS implementation fails the same
fixture identically. With only that fixture omitted in an external runner,
all remaining suite checks pass, including x86_64/i386, Android NDK loader
builds/packaging, resident adoption, musl/bwrap/ldconfig, apply-extra, and
rewrite tests. The prior apply-extra exit 4 does not reproduce here.
Logs and baseline strace: `/mnt/data/aosp/out/pc-logs/fork-adopt/r5-*`,
`baseline-zygote.log`, and `baseline-trace.*`. No complete-suite pass is
claimed while the independent zygote credential fixture remains failing.

Steam ldconfig follow-up (2026-10-27): the night-r5 trace already captures
bwrap's stderr, interleaved with syscall diagnostics: `Can't create file
/app/lib/i386-linux-gnu/GL: Read-only file system`. The child fails during
bwrap setup before executing ldconfig. Operation 27 is close (`-9` is EBADF
from bwrap's fd cleanup), and 135 is ioctl, not personality.

The optional Compat.i386 preparation path now pairs its EROFS mount with
`--erofs-backing` for the disposable Flatpak files directory, just as declared
packages do. Stock bwrap binds that directory under both /app and /usr;
resolving it to the image lets the existing per-launch destination shadows
handle its tmpfs/GL32 mountpoints. No personality policy or per-app rule is
needed. The normal immutable payload view stays read-only.

`tests/flatpak-bwrap-host.py` now reproduces the missing-backing setup failure
and verifies that the mapping lets ldconfig write a nonempty cache under the
Android filter plus Flatpak's payload filter. It also passes with
`COMPAT_HOST_RUNTIME` pointing at the extracted FreeDesktop 26.08 runtime in
`/mnt/data2/matonos-host-smoke/mnt/matonos/1000/`.
The eight PackageDeployment Java tests pass, including the optional-extension
argument regression. The Steam host smoke passes its ldconfig probe and
continues beyond cache creation under the pre-handler filter; Steam later
exits 71 without a diagnostic, which remains unresolved.

Device recheck: rebuild/update the subsystem APK (the fix is in Java, so a
loader-only replacement is insufficient), relaunch Steam on the coordinator's
fresh image with v3-trace enabled, and retain the full stderr stream. Verify
that bwrap resolves Compat.i386 files to its /mnt/matonos image, creates the
GL tmpfs destination, executes ldconfig, and no longer reports exit status
256. No VM was used for this follow-up; no kernel or shared image policy
changes are required.

Round 11: Zypak spawn-request ancillary truncation
------------------------------------------------
The day-r10 trace shows pid 16453 successfully sending 552 bytes and 125
SCM_RIGHTS descriptors on fd 9. Supervisor pid 16427 receives 552 bytes on
fd 21 but only 16 descriptors, then prints EMSGSIZE. This is Zypak's
userspace `Socket::Read` rejecting MSG_CTRUNC, not a kernel sendmsg failure.
Its spawn strategy enumerates `/proc/self/fd` and forwards every entry;
its receiver allocates space for 16 descriptors.

Our enumeration filter excluded reserved proxy descriptors but omitted
resident Android EGL/driver descriptors. They survive emulated Linux exec
for the Android libraries, so forwarding them as Linux child descriptors
inflates that request. Enumeration now also excludes resident-owned fds.
This changes neither the reported fd limit nor application descriptor numbers,
and does not close driver fds or increase Zypak's buffer. Explicit application
replacement of a resident fd still retires its ownership as before.

The resident-fds host fixture now creates 125 resident driver fds and runs
Zypak's enumerate/send/receive pattern through the actual installed proxy.
Before the fix it fails on MSG_CTRUNC; afterward the fixed 16-fd receiver
gets every enumerated application fd. Existing fork, dup2 replacement,
close_range and resident exec checks remain in that fixture.

Device validation remains necessary: on a fresh coordinator image launch
stock Obsidian with v3-trace and capture logcat through the first window or
90 seconds. Look for `proc-resident-fds-hidden` (includes pid/count), the
spawn-request send/recv `unix-io` lines (now include pid and msg_flags),
absence of Zypak's EMSGSIZE, and a display socket connection/toplevel.
The device log did not contain fd identities, so matching all 125 device
fds to resident ownership is an inference to verify with that capture.
Also re-run Kate to retain its working posix_spawn/vfork coverage. No VM,
AOSP build, shared device-tree edit or kernel change was made for this fix.

Host validation: the complete `bash tests/run.sh` passes, including the
previously failing apply-extra fixture; `python3 tests/unix-trace-host.py`
passes with pid/message-flag assertions. Optional root-owned marker,
i386-musl and captured EROFS ldconfig fixtures retain their existing skips.
Evidence: `/mnt/data/aosp/out/pc-logs/fork-adopt-r11-{baseline,focused,suite,final-suite}.log`.
Upstream protocol references: Zypak `src/sandbox/spawn_strategy/run.cc`
(`FindAllFds`) and `src/base/socket.cc` (`Socket::Read`) at
https://github.com/refi64/zypak/tree/master/src .


Steam round 8: continuous descendant reaping (2026-10-27)
-------------------------------------------------------
The launch supervisor previously blocked in wait4(main) until Flatpak's
main process exited. Its adopted descendants could therefore remain zombies
throughout a live session. It now blocks in waitid(P_ALL, WEXITED), consuming
all direct/adopted child exits immediately, retrying EINTR and stopping at
ECHILD. The main child's status is recorded separately and remains the
application result; living descendants still keep the supervisor alive.
This is driven by kernel child-state notifications, without a polling loop
or an asynchronous signal handler. Children with a living payload parent
are not eligible for this supervisor's wait and keep their original status.

The regression creates a short-lived parent, lets its orphan exit 37, and
checks /proc/<orphan>/status disappears while the main child is still alive.
It then exits the main child with 23 and verifies that status is preserved.
The pre-change supervisor fails the live-child test with fixture exit 16;
x86_64 and i386 fixed supervisors pass under the Android filter. Existing
checks still verify that descendants outlive a successfully exited main.

Validation: the full compat-proxy tests/run.sh passes, including Android NDK
builds and packaging. Evidence: out/pc-logs/steam-personality/r8-final-suite.log
in the AOSP checkout. No VM was started.

Steam round 8: redirected helper errors
--------------------------------------

Zenity investigation: day-r10/steam.txt identifies PID 16746 (0x416a) as
/usr/bin/zenity, launched after steam-runtime-check-requirements PID 16737.
It receives self-sent SIGABRT at libc+0x75f3c after two successful writes
of 462 and 469 bytes. Their text and destination fd are absent from the old
trace, so neither a GTK/D-Bus assertion nor a rendering failure is proven.
The trace has no steamwebhelper exec. The screenshot alone cannot identify
the owner of the black surface, and no evidence ties the adopted zombies
to that surface: their original parents have already exited.

Opt-in trace now copies successful fd-2 write/writev bytes to the preserved
proxy diagnostic sink, labelled payload-stderr with PID and byte count.
This retains GTK/GLib errors even when the launcher redirects stderr into
a file or /dev/null. It preserves the original write result and destination;
normal launches do not duplicate payload output. The regression redirects
stderr to /dev/null and verifies both write forms are visible only with
--trace, including execution through the i386 peer loader.

Coordinator's next capture (no agent VM was started): rebuild the subsystem,
launch stock Steam on a fresh image with v3-trace, and save the full logcat
stream through zenity's exit. Retain payload-stderr begin/end blocks for
its PID, supervisor-descendant-status events while main is alive, and the
Steam logs directory. To attribute the surface, include the compositor's
window PID/title list at the black window. The abort reason and black-window
cause remain open; this change fixes continuous reaping, not rendering.
No shared device-tree or kernel changes are required.

The later host attempt with the captured FreeDesktop 26.08 runtime, Android
filter and Xvfb did reach the same libc+0x75f3c abort. Its stderr identifies
Gtk gtkiconhelper.c:495 ensure_surface_for_gicon: glycin cannot load GTK's
fallback image-missing.png because spawning its image decoder fails. In
this host fixture glycin attempts bwrap and gets ENOENT (the fixture maps
/usr to the runtime, which lacks that host bwrap). The device instead sees
/.flatpak-info and successfully execs /bin/flatpak-spawn twice, PIDs 16756
and 16762, immediately before the abort. Thus the host ENOENT is not itself
a proven device bug; the matching image-loader failure is the specific
lead for the next capture. Preserve zenity's complete assertion text and
both flatpak-spawn children/portal stderr. Do not disable glycin isolation
or add a Steam override to bypass that failing nested launch.
Host evidence: out/pc-logs/steam-personality/r8-host/zenity.log (the attempt
hit its 180-second timeout while processing the slow archived runtime;
it is not a passing GUI test). All new test output/scratch was under out.

Steam round 9: host identity and glycin spawn (2026-10-27)
---------------------------------------------------------
The day-r12 capture proves the pre-bwrap Flatpak CLI asks getpwuid_r() for
the stub's Android UID (10137), but the host `/system/etc/passwd` has no such
entry. The v3 launcher now maps a generated passwd/group view over both
`/etc` and its canonical `/system/etc` paths. Its passwd home is the canonical
stub `home` directory, which is also the launch HOME. bwrap overlays its normal
runtime passwd/group files inside the application sandbox. Host regressions
check account serialization and getpwuid() through the proxy's Android filter.

The same capture proves zenity aborts because its glycin decoder exits 1 when
started by `flatpak-spawn --sandbox`; it reports the missing GTK fallback image
as a consequence. It records both helper execs and the forwarded fd, but no
portal Spawn reply, nested bwrap stderr, or helper exit diagnostic. The reason
for the nested launch failure is therefore still unproven; do not disable
glycin isolation or add a Steam-specific override. The next device capture
should retain upstream Flatpak portal debug output plus the nested bwrap argv,
stderr and exit status for both helper calls. No VM was started for this round.

Steam round 10: portal default installation (2026-10-27)
--------------------------------------------------------

The day-r13 trace captures both `flatpak-spawn --sandbox` helper launches.
The portal's stock `flatpak run` child checks the default system installation
at `flatpak-launch/system/app/com.valvesoftware.Steam/...` and reports the
installed commit as missing. The app is launched from the private user
installation, so its signed deployment metadata was only present at
`flatpak-launch/app/...`. This causes the Steam runtime's display probe to
fail and prevents the glycin image decoder from starting; zenity then aborts
while loading GTK's fallback image. `steamwebhelper` is never exec'd in this
capture, so the black window's later-stage cause remains unobserved.

PackageDeployment now publishes the app's deployment record into both private
roots, with both pointing to the same package-owned EROFS mount. A regression
checks the stock `active` link, deployment metadata, deployed-ref index, and
OSTree commit object in both roots. Re-run the Round 10 fresh-image capture to
confirm the portal probes and glycin decoder launch, then inspect whether
`steamwebhelper` starts and connects to the display. No VM was started for
this change.

Steam round 11: nested stock Flatpak installation view (2026-10-27)
-------------------------------------------------------------------
The day-r14 trace shows Steam's final pre-webhelper display probe going
through the upstream Flatpak portal's `Spawn`: it runs `flatpak run` for the
same app with `echo "${DISPLAY+DISPLAY_is_set}"`. That nested Flatpak process
reports the app commit as not installed. The trace shows why the Round 10
metadata duplication alone was insufficient: lookups of both
`flatpak-launch/app/...` and `flatpak-launch/system/app/...` resolve as
missing from inside the caller's `--nofilesystem=host` sandbox. Steam then
shows the generic D-Bus/portal error and never executes `steamwebhelper`.

The launcher now explicitly exposes only that launch's generated Flatpak
installation tree read-only to its own sandbox. Stock Flatpak and the stock
portal keep their normal lookup and Spawn behavior; the portal's nested CLI
can read the already materialized refs through the normal filesystem view.
The regression verifies that the launcher grants exactly the generated root
as `:ro`. The host smoke installed and packed Steam, and its `ldconfig` probe
passed under the Android pre-handler filter. Its separate `steam --version`
probe stopped at the host adapter's missing `/usr/lib/os-release` symlink,
before testing the portal lookup. The subsystem Java unit suite, repository
host suite and compat-proxy suite passed. Re-run Steam on a fresh device image:
confirm the nested display probe exits successfully, `steamwebhelper` executes,
and it connects to the Wayland/X11 display. This round used the coordinator's
trace and did not start a VM.

Obsidian round 16: APK bwrap filter activation
---------------------------------------------

`day-r15/obsidian-threads.txt` shows browser 28745 sleeping in recvmsg on fd
20 (`__skb_wait_for_more_packets`), its sandbox IPC thread polling, and the
unsandboxed zygote 28772 polling. These waits persist through the available
samples (t=23; subsequent headings contain no process samples). The nested
portal launch is the concrete failure: flatpak 28778 runs bwrap 28779 for
ldconfig, whose clone is rejected by `flatpak-seccomp-deny 56 -1`; bwrap
reports no permission to create a namespace and Flatpak reports ldconfig
status 256. The browser subsequently reports failure to notify the stub of
exit (EBADF) and remains waiting. No display connection or inotify EOF
error explains this capture.

The bwrap detector omitted the APK executable name
`libmatonos-app-bwrap.so`, although the argument logger already recognised
it. Consequently the monitor installed and activated its payload filter
immediately instead of deferring it until payload exec. Monitor descendants
then inherited a filter forbidding their namespace setup. Recognise this
packaging name in the existing detector; retain normal filter inheritance
and enforcement for the actual payload. This is independent of app identity.

`tests/bwrap-filter-name.c` execs the APK-named monitor, installs a filter
forbidding unshare and user-namespace clone, performs monitor namespace
setup and clone, execs the payload, and verifies its unshare is denied.
It also verifies the monitor remains usable after the payload exits.
The pre-fix proxy aborts this regression (SIGABRT); the fixed proxy passes.

Device verification remains necessary: rebuild the proxy in a fresh image,
launch stock Obsidian with v3-trace for 90 seconds, and capture logcat and
window state. Confirm the nested bwrap no longer emits the namespace/ldconfig
failure, the sandboxed zygote executes, and a display connection/toplevel
appears. Recheck Kate rendering. No VM or image build was started here.

Validation: full `bash linux/compat-proxy/tests/run.sh` exits 0, including
mixed x86_64/i386, staged musl bwrap and apply-extra. Logs in the AOSP checkout:
`out/pc-logs/fork-adopt-r16-suite.log`, `fork-adopt-r16-focused.log`, and
`fork-adopt-r16-baseline.log`. No shared or kernel changes are required.

Obsidian round 17: inherited filter during nested bwrap setup
------------------------------------------------------------

The absolute `day-r16/obsidian.txt` capture still fails before ldconfig:
portal-launched Flatpak 4122 execs APK-named bwrap 4123, which receives
`flatpak-seccomp-deny 56 -1` on its namespace clone. It prints the namespace
permission error and exits 1; Flatpak reports `ldconfig failed, exit status
256`. Thus round 16's spelling fix did not resolve the captured blocker.
The browser's sandboxed zygote cannot finish launching, before any display
connection. This is distinct from the inotify EOF fixed on main.

The monitor already inherits an ACTIVE outer Flatpak payload filter. Deferring
only filters newly installed by that monitor cannot undo its inherited clone
denial. For a bwrap monitor, dispatch namespace clone, unshare, mount, umount
and pivot_root to the existing virtual namespace implementation before the
payload-filter ERRNO check. These operations construct the proxy's virtual
view; clone still strips kernel namespace flags. Ordinary payloads keep the
inherited policy, and this does not remove the kernel's inherited filter.
There are no application-specific conditions.

The APK-name regression now installs the outer filter before execing the
monitor, then installs the nested filter, exercises namespace setup and clone,
and verifies unshare remains denied after payload exec. The same regression
aborts on unmodified main at the monitor's namespace setup assertion and
passes with the fix. Full compat-proxy host suite (including apply-extra)
and Android x86_64/i386 builds passed (exit 0); results are recorded in `out/pc-logs/fork-adopt-r17-{suite,android}.log`.

Next device verification: fresh image, stock Obsidian with v3-trace for 90 s
and window state; confirm nested bwrap no longer reports clone denial or
ldconfig status 256, `/app/bin/zypak-helper child` and the sandboxed zygote
execute, then inspect display connect/get_toplevel. Recheck Kate rendering.
This change fixes the evidenced setup failure; a window is not yet verified.
No VM, AOSP build, shared-file changes or kernel changes were made.

Obsidian round 18: inherited virtual user-namespace limit
-------------------------------------------------------

In `day-r17/obsidian.txt`, nested bwrap 6175 returns EPERM from clone
(operation 25) and prints the namespace permission error. It never execs
ldconfig; status 256 is bwrap's exit 1. There is no payload-filter denial.
Outer bwrap used `--disable-userns` and accessed max_user_namespaces. Our
virtual limit survives exec and supplies a second rejection after round 17's
filter exemption. Browser 6144 starts unsandboxed zygote 6168, but portal
Flatpak 6174 fails its ldconfig setup before the sandboxed zygote can exec.
The captured `--type=zygote` argv for 6174 is an argument to `flatpak run`,
not evidence that the sandboxed Electron zygote executed. No GPU/renderer
exec or display connection follows; the browser polls after the spawn failure.

Allow recognized bwrap monitors to perform virtual user-namespace setup
with an inherited zero limit, matching their existing inherited-filter
exemption. Preserve zero for payloads and prevent decrement underflow.
A fresh sysctl write by the monitor clears the exemption, so upstream
bwrap can verify its own --disable-userns request. The initial broad
exemption failed apply-extra on that check; the narrowed fix includes a
regression assertion for it. Kernel namespace flags continue to be stripped.
No application rule is used.
The APK-name regression now carries an outer `--disable-userns`-style zero
limit through exec in addition to the inherited Flatpak seccomp filter.
Unmodified main aborts on its monitor's unshare assertion; the fix permits
monitor unshare/clone and payload exec while payload unshare remains EPERM.

Device verification still needed: fresh image, stock Obsidian with v3-trace
for 90 seconds and window state. Check that bwrap's namespace error and
ldconfig status 256 disappear, then look for actual `zypak-helper child` /
Electron sandboxed zygote exec, GPU/renderer and display connect/get_toplevel.
Recheck Kate rendering. No VM, AOSP build, shared-file or kernel change.

Validation: complete `tests/run.sh` passed (exit 0), including apply-extra,
i386/cross-exec, Android packaging and rewrite tests; standalone Android
x86_64/i386 builds passed. Logs: `out/pc-logs/fork-adopt-r18-suite.log` and
`fork-adopt-r18-android.log`. Two earlier suite attempts hit the existing
zygote SOCK_STREAM/credentials framing fixture; the identical failure was
reproduced with unmodified main (`fork-adopt-r18-main-zygote.log`), and the
final full run passed it. Captured EROFS/i386-musl fixture skips remain as
reported by the suite. Device window rendering remains unverified.
Steam round 12: nested display probe and last child (2026-10-27)
---------------------------------------------------------------
The day-r15 trace confirms the portal's nested `flatpak run` reaches the
display probe command (`/bin/sh -euc 'echo "${DISPLAY+DISPLAY_is_set}"'`),
but bwrap exits before running that command: its stderr says it cannot create
a new namespace because unprivileged user namespaces are unavailable. So the
probe is attempted, not successful, and it does not establish whether the
display socket works. Steam then starts `zenity` with the generic D-Bus / portal
error. Zenity aborts because its glycin image loader exits early; the trace
shows the nested `flatpak-spawn ... prlimit ... glycin-image-rs` request and a
loader failure, followed by Steam's `xterm: command not found` fallback. The
last traced helper is `cat /tmp/tmp.2frL9mdVbV`; no `steamwebhelper` exec is
present. The capture does not include that helper's collected exit status, so
it does not yet establish whether Steam waits on `cat` or what it reads.

The proxy trace does not report the parent application's wait status because
wait4 is passed through the Android app filter without entering the proxy
handler. The trace proves the namespace probe blocker and zenity's GTK loader
failure, but does not prove either is the final reason Steam does not reach
`steamwebhelper`. Re-run a fresh-image Steam launch with v3-trace and payload
stderr enabled. While the window is black, capture the Linux process tree and
for the `cat` PID and its parent capture `/proc/<pid>/{status,wchan,syscall}`
and `/proc/<pid>/fd` symlink targets; preserve the temporary file's contents
and Steam's `~/.var/app/com.valvesoftware.Steam/logs/`. This will distinguish
a still-running `cat`, a completed child whose parent is blocked elsewhere,
and a parent waiting on another helper. No VM was started for this diagnostic
round.

Steam round 13: GUI launch stdin EOF (2026-10-27)
-------------------------------------------------
Rebased on main, preserving both the Obsidian round 16/17 notes and Steam
round 12 notes in the README conflict. The day-r17 process snapshot identifies
`pipe:[1198044]` as stdin for the loader/Flatpak/Steam tree. The Java stub
7121 retains fd 203 for that pipe: this is ProcessBuilder's parent-side stdin
writer, exposed as Process.getOutputStream(). V3Launcher never wrote to or
closed it. No proxy-reserved duplicate of that pipe appears in the snapshot.
The other holders shown are stdin readers, including Steam 7176 and 7351.

7351 has no exec-argv event: it forks from 7176's bash running steam.sh
(`-srt-logger-opened -no-cef-sandbox`). It first forks 7352, which execs
`cat /tmp/tmp.TWgk5hayqP`, then remains in read(0) while 7176 waits for it.
This is consistent with the shell's error-dialog fallback reading stdin:
zenity 7323 aborts (steam.sh line 103), xterm 7350 is missing (line 138),
and 7351 writes 25 bytes before blocking. The trace does not record the shell
builtin's source line or buffer contents, so those details are inference.
The last exec is cat, not steamwebhelper; 7351 itself is the inherited shell.

GuiProcess.start now starts the GUI child and immediately closes its stdin
writer, on the persistent spawner thread. GUI launches have no input producer;
EOF is the normal noninteractive result, propagated to all inherited readers.
It destroys the child if closing the writer fails. V3Launcher uses this common
launch helper; no Steam-specific rules or proxy syscall changes are needed.
Stdout/stderr draining and the supervisor lifetime remain unchanged.

The regression uses a shell parent waiting on a forked cat reading stdin.
Its control verifies an open Java writer blocks; closing it releases both
processes. The production helper must complete the same command with EOF.
It runs directly in tests/run.sh and through the proxy supervisor with the
Android filter in compat-proxy/tests/run.sh. Focused runs pass in both modes.

Fresh-image check for the coordinator (no VM run here): launch stock Steam
with v3-trace, capture stderr and process/fd state after 10 and 30 seconds.
Verify the Java stub no longer holds the app stdin pipe writer, the error
fallback reader does not remain in anon_pipe_read, and the parent continues
or exits. This fixes the evidenced EOF deadlock; it does not establish that
Steam reaches steamwebhelper. Preserve any requirement/dialog error now
printed before exit: zenity's abort and missing xterm preceded this wait.
No shared device-tree changes or kernel changes are required.

Validation: repository host suite, Android `compileDebugJavaWithJavac` and
`testDebugUnitTest`, and device-tree preflight passed. The full compat-proxy
suite passed on retry; its first run failed in the existing zygote-startup
boot framing check (13 bytes instead of the expected 12), before the new test.
A copied helper retaining the old open writer fails the new regression with
`stdin EOF never reached child`. Logs are under the AOSP checkout's
`out/pc-logs/steam-personality-r13/` (`host-tests.log`, `android.log`,
`proxy-tests.log`, `proxy-tests-retry.log`, `negative.log`, `preflight.log`).
Android validation compiled Java and ran unit tests without native preBuild
tasks; this change adds no native code. No host-smoke installation or VM was
used in this round.

### Obsidian nested bwrap assertion (fork-adopt round 19)

The day-r18 device trace confirms the nested ldconfig phase finishes without
`ldconfig failed`. Browser 8809 starts unsandboxed zygote 8833 and sandbox
shim 8835. The portal launches Flatpak 8839, which starts nested bwrap child
8846 for `zypak-helper child - /app/obsidian --type=zygote`. Its argument
bundle contains `--assert-userns-disabled`. At 18:47:12.128 its unshare
probe incorrectly returns success (`ours 367 0`), then it prints
`creation of new user namespaces was not disabled as requested` and exits.
The browser subsequently reports the stub exit notification failure. There
are no GPU/renderer execs or display connects in this capture.

The inherited-zero exception added for nested monitor setup was too broad:
it also allowed the unshare probe that checks the outer bwrap's zero limit.
Restrict the exception to namespace clone setup. Unshare observes the zero
limit, including inside a nested monitor. A monitor that explicitly writes
its own nonzero limit can still unshare, then set zero and verify denial.
This changes the proxy's general namespace emulation, with no app rules.

`tests/bwrap-filter-name.c` now covers inherited-zero unshare denial before
any monitor sysctl write, initial clone setup, payload exec/filter enforcement,
and explicit nonzero/zero monitor limits. Restoring only the old unshare
condition makes it fail at the inherited-zero assertion; the fixed test passes.

Coordinator fresh-image check: launch stock Obsidian with v3-trace for 90 s;
retain exec-argv, bwrap argument bundles, stderr and Unix I/O. Verify the
nested `--assert-userns-disabled` probe returns EPERM, its diagnostic is gone,
and `zypak-helper child` reaches the sandboxed zygote. Then inspect GPU/renderer
startup and Wayland/X11 connects/toplevel creation. Recheck Kate as control.
No VM was run here; this fixes the evidenced bwrap exit, and does not establish
that Obsidian renders. No shared device-tree or kernel changes are needed.

Round 19 validation: full `bash linux/compat-proxy/tests/run.sh` passed
(including x86_64/i386 builds, musl/stock bwrap, apply-extra and site rewrite
fixtures); device-tree preflight passed. Logs:
`/mnt/data/aosp/out/pc-logs/fork-adopt-r19-tests.log` and
`/mnt/data/aosp/out/pc-logs/fork-adopt-r19/{positive,negative,preflight}.log`.

### Round 20: distinguish nested setup from the userns assertion

`day-r19/obsidian.txt` identifies pid 10211 as the portal-side nested
ldconfig bwrap (`--args 31 ldconfig -X -C ...`). Its argument bundle has
no `--assert-userns-disabled`. The inherited-zero check in the emulated
unshare path nevertheless rejected its setup with EPERM. The prior clone
exemption alone could not cover bwrap's subsequent unshare.

Nested bwrap monitor setup now has the same inherited-limit exemption for
unshare as clone. Explicit `--assert-userns-disabled` monitors retain the
zero-limit probe behavior. Options are recognized in direct argv and the
NUL-delimited `--args` fd, before loader replacement or CLOEXEC cleanup.
The limit remains zero for ordinary app payloads; a monitor writing its
own limit still loses the exemption. This models portal-side setup outside
the app restriction without enabling userns creation in the app itself.

The bwrap-filter-name regression starts with an outer zero limit and active
Flatpak seccomp, then forks assertion monitors (direct and bundled option),
and a setup monitor. Assertions get EPERM; setup unshare and clone succeed;
execed app unshare and monitor-written zero limits remain denied.

Device follow-up (coordinator; no VM used here): fresh image, Obsidian with
v3-trace for 90 seconds. Check that nested ldconfig no longer prints
`unshare user ns`, both ldconfig runs finish, and inspect browser/zygote,
gpu/renderer exec and display socket connect/toplevel events. Keep Kate as
control. No shared device-tree or kernel changes are needed.

Validation: complete `bash linux/compat-proxy/tests/run.sh` passed (exit 0),
including x86_64, i386, cross-exec, Android loader packaging, apply-extra
and site rewrite. Host log: `/mnt/data/aosp/out/pc-logs/fork-adopt-r20-host.log`.
Device-tree preflight also passed. The focused regression was extended to
check unshare in the cloned monitor child before payload exec and passed
again (`fork-adopt-r20-regression.log`).
### Round 21: devpts remapping precedes the userns assertion

The existing `day-r20/obsidian.txt` capture already includes the full bundle
for PID 10947, fd 68 (19:07:25.670). It has `--assert-userns-disabled` and
`--dev /dev`, but **no `--userns`, `--userns2` or `--pidns` fd**. Thus
`--parent-expose-pids` in Flatpak's argv did not result in joining those
namespaces in this capture. Do not assume that argv flag proves setns ran.

Upstream bubblewrap's non-root path implicitly adds CLONE_NEWUSER. With
`--dev`, its first namespace maps the real IDs to zero to mount devpts.
After pivot_root it calls unshare(CLONE_NEWUSER) to restore the requested
UID/GID, then calls unshare again for `--assert-userns-disabled`. Round 20
rejected the first of these two calls, producing `unshare user ns`, before
it ever reached the assertion. The previous synthetic test omitted devpts
and therefore modeled the first unshare as the assertion itself.

Recognized assertion monitors now get one devpts remapping transition when
creating their own user namespace with nonzero requested IDs. Option
inspection handles direct argv and NUL-delimited `--args` fds, including
explicit `--uid`/`--gid`; joining a supplied namespace or using the external
userns-block protocol does not grant that transition. Successful unshare
consumes it. The inherited zero limit, subsequent assertion, monitor-written
limits and payload filter remain intact. This extends the existing virtual
monitor setup model; it does not create kernel namespaces or introduce an
application-specific rule.

`tests/portal-userns.c` replays the portal boundary as `flatpak run --sandbox
--parent-pid=... --parent-expose-pids`, then executes the **shipped stock
bwrap** with `--args 68` under the inherited app seccomp filter and zero
userns quota. Only Flatpak's deployment lookup/argument assembly is an
offline fixture: no installed Flatpak deployment is altered. The bundle
contains the captured devpts/assertion path and the real bwrap forks,
unshares, asserts, and execs a payload that verifies the inherited filter.
This regression fails on unmodified main with the same `unshare user ns`
error and passes with the fix. `tests/musl-host.sh` runs it, and the smaller
APK-name regression also covers both unshares in order.

Device rerun (coordinator; no VM was used here): fresh image, stock Obsidian
with v3-trace for 90 seconds and window state, plus Kate as a control. Expect
`bwrap-devpts-userns` once in the nested child, then a successful unshare,
the assertion's EPERM, and `zypak-helper child` payload exec. Check whether
GPU/renderer processes and a display connection/toplevel follow; this host
regression does not establish that Obsidian renders on device. No shared
MatonOS device-tree or kernel changes are needed.

Validation: `tests/run.sh` passed completely (exit 0), including apply-extra,
new portal regression, mixed i386/x86_64 exec and rewrite suites. The final
standalone APK-name and real-bwrap portal regressions passed too. Android
x86_64 and i386 builds via `build-android.sh` passed. Evidence is in
`/mnt/data/aosp/out/pc-logs/fork-adopt-r21-{suite,android,name-final,portal-final}.log`;
the baseline's matching bwrap failure is in `fork-adopt-r21-portal-before.log`.
The branch was subsequently rebased onto main `6705c95` (the coordinator's
Steam revert). Both complete `tests/run.sh` and Android x86_64/i386 builds
passed again on that base; final logs are `fork-adopt-r21-rebased-suite.log`
and `fork-adopt-r21-rebased-android.log` in the same directory.
Steam round 14: nested tmpfs descriptor exhaustion (2026-10-27)

`day-r18/steam.txt` identifies the actual wrapper probe as
`steam-runtime-launch-client --bus-name org.freedesktop.portal.Flatpak
--app-path '' --share-pids -- /bin/sh -euc 'echo "${DISPLAY+DISPLAY_is_set}"'`
(pid 9133). The upstream per-stub portal receives it and launches Flatpak
9136 / bwrap 9140. That bwrap fails mounting tmpfs on `/dev`: operation 183
returns -24 (EMFILE), and stderr says `No file descriptors available`.
The subsequent glycin portal children fail identically; zenity then aborts
because its error icon cannot be decoded. This is not evidence of an absent
D-Bus service, despite the wrapper's generic error message.

Every synthetic tmpfs retains a flock descriptor, inherited through nested
launches. The private allocator previously rejected the eighteenth used
reserve slot even with ordinary descriptor numbers available. Private spill
tracking now grows with actual demand, survives resident exec, and participates
in the existing close/dup/limit ownership handling. The actual kernel fd limit
still applies. No Steam-specific or portal protocol changes are needed.
`tests/mounts.c` creates 64 tmpfs mounts, execs, and creates another one.

The proc-fd regression also opens a pipe through `/proc/self/fd/N`, then
reads the alias link. Non-file proc magic labels must remain kernel links
when opened, and `readlink` must return raw `pipe:[N]` rather than a tracked
pathname. Reverse translation now explicitly preserves nonabsolute labels;
proc fd metadata lookup excludes FIFO, socket and anonymous-inode labels,
while retaining pathname metadata for real device nodes.

Device retest: launch Steam with payload stderr and exec-argv tracing; verify
the display probe executes `/bin/sh`, prints DISPLAY_is_set, and exits zero;
verify nested bwrap no longer reports EMFILE or ldconfig status 256. Then
check whether steamwebhelper starts and the window draws. No VM was run here.

Validation: production `build-android.sh` and device-tree preflight pass.
The mount/exec and pipe-alias regressions pass, as do static musl bwrap,
Flatpak-shaped compat-ldconfig/ldconfig/payload, the remaining x86_64 proxy
suite, i386 P1/dynamic/cross-exec and rewrite tests. Full `tests/run.sh`
stops in the existing i386 supervisor case: `supervise.py` compiles its child
as x86_64 and asks the i386 loader to load it (`load-error -1`). The x86_64
supervisor cases pass; the remainder was run separately. The Chromium zygote
credential fixture failed once and passed on immediate retry. Logs are in
`/mnt/data/aosp/out/pc-logs/steam-personality-{suite-remainder,suite-tail,musl,paths,android-final,preflight}.log`.

Fresh Steam host smoke completed in the isolated directory
`/mnt/data2/matonos-host-smoke/steam-personality-r14-20261027`:
`probe ldconfig: PASS`; full launch and `steam --version` exit 71. Its
report records no blocked syscall, with `/etc/os-release` containing the
relative link text as its first diagnostic. This smoke does not establish
that the on-device portal probe or Steam UI now succeeds. See that directory's
`host-smoke.md` and `logs/com_valvesoftware_Steam.launch.log`.

### Steam round 15: named socket bind regression

Restored 2bd9538 on main's revert, retaining the growing private-fd tracking
and anonymous proc-label handling. The regression was the blanket S_ISSOCK /
S_ISFIFO exclusion in vfs_proc_fd_name: musl bwrap resolves its source using
an O_PATH fd. A named socket is still a filesystem object, so that descriptor
must keep its tracked virtual name. Actual socket/pipe I/O descriptors still
return the kernel's socket:[N] / pipe:[N] labels.

`tests/bwrap-socket.py` maps a directory containing a real Unix socket to
/tmp/.X11-unix, then runs the staged stock musl bwrap with a socket --bind.
It fails on restored 2bd9538 with the device's exact ENOTDIR error and passes
with the O_PATH distinction. The host distro's bwrap alone did not reproduce
this; musl-host.sh now runs the regression after staging the shipped applet.
No per-app rules, AOSP changes, or kernel changes are needed. Device follow-up:
fresh-image Kate launch, then Steam launch; verify the X11 bind succeeds and
capture any remaining portal probe failure. No VM was used for this round.

Validation: native proxy build passed; full tests/run.sh passed through the
musl Flatpak ldconfig/payload and apply_extra checks, including the new socket
regression, 64 tmpfs locks across exec, and pipe aliases. It stopped at
loader-maps.py's Private_Dirty == 0 assertion (504 kB text mapping); an isolated
rerun reproduced that failure. This round does not claim a fully green suite.
The remaining tests/site-rewrite.sh passed separately. Logs are in
/mnt/data/aosp/out/pc-logs/agents/steam-personality-r15-{tests,maps,rewrite}.log.

### Proc fd object checks (fork-adopt round 22)

The GPU fatal check in `day-r22/obsidian.txt` occurs after Chromium opens
`/proc` and `self/fd/`, enumerates descriptors, and stats fd 1. The resolver
returned the pipe's readlink label (`pipe:[1299974]`) as a pathname, so
`fstatat` returned ENOENT. Opening the proc/task/fd directories had succeeded;
the failure was following an individual proc fd magic link.

For pipe/socket/anonymous objects and deleted files, keep the native proc fd
magic-link pathname and let the kernel follow the object. Continue resolving
ordinary file targets to their backing path, which bwrap uses to identify
bind-mount destinations. This is independent of the application and requires
no Flatpak or AOSP changes.

`tests/proc-dirfd.c` repeats the task/fd dirfd opens and Chromium's enumerate
and fstatat loop, comparing each result with fstat; it also checks no-follow
symlink metadata and reopening a deleted memfd. `tests/proc-dirfd-host.py`
runs it inside two shipped bwrap invocations with `--proc /proc` and inherited
Flatpak BPF. Both reproduce ENOENT with main f173d3f and pass with this fix.
The Flatpak-shaped payload fixture runs the check too. Validation passed:
`tests/run.sh` (including x86_64/i386 Android loader builds, mixed-class exec,
Flatpak/musl, apply-extra and rewrite tests), `tests/musl-host.sh`, and native
and proxied proc-dirfd runs. Suite log:
`/mnt/data/aosp/out/pc-logs/fork-adopt-r22-suite.log`.

Device validation remains with the coordinator: freshly launch Obsidian with
v3-trace and capture its log plus compositor toplevel events. Verify the
`proc_util.cc` fatal and GPU retry loop disappear and a window appears; use
Kate as the rendering control. No VM was run for this change.

### Payload re-exec through /proc/self/exe (fork-adopt round 23)

The GPU-process loader failure was caused by publishing the exec request's
literal `/proc/self/exe` as the new payload identity. Opening the ELF already
followed that magic link correctly, but AT_EXECFN and the subsequent emulated
readlink exposed the alias. glibc consequently searched `/proc/self` for
`$ORIGIN` libraries such as libffmpeg.so. `proxy_load` now substitutes the
saved payload pathname for self proc exe aliases before loading, state handoff,
auxv construction and identity publication; argv is preserved. This applies to
all payloads, with no application-specific rules.

`tests/exec-origin.sh` builds an ELF with a mandatory adjacent shared library
and RPATH `$ORIGIN`, maps it at `/app/payload`, and re-execs `/proc/self/exe`.
It checks readlink and AT_EXECFN, both directly and through nested bwrap with
an inherited Flatpak seccomp filter. The previous loader exits 127 on the same
fixture because the adjacent library cannot be found; the fixed loader passes.

Device validation (coordinator; no VM run by this agent): launch Obsidian with
v3-trace and capture payload stderr, exec-argv and compositor events. Confirm
GPU self-execs no longer report missing libffmpeg.so, then check whether a
Wayland/X11 connection and toplevel appear. Re-run Kate as the control.

Validation: native x86_64/i386 builds and full `tests/run.sh` passed, including
Android loader packaging, mixed-class exec, Flatpak/musl, apply-extra and rewrite
checks. Suite log: `/mnt/data/aosp/out/pc-logs/fork-adopt-r23-suite.log`.
Both direct and nested regressions fail with exit 127 using the pre-fix loader;
the fixed versions pass. No image, kernel or shared-device-tree changes needed.
### Steam round 16: preserve nested launch mount precedence

Rebased on main. The supplied day-r23/steam.txt does not contain an X11
ENOTDIR failure: its nested socket bind succeeds at 19:34:33.541. The
recorded failure is pid 13567 opening /dev/shm/flatpak-com.valvesoftware.Steam-J182X3
at 19:34:33.995 (ENOENT). Flatpak created it beneath the outer sandbox's
private shared-memory directory, but bwrap's initial pivot exposed the older
bootstrap /dev/shm instead. The trace shows the newer oldroot/dev/shm alias
being overwritten by the bootstrap alias at 19:34:33.550.

Recursive bind and pivot snapshots now omit covered mounts at identical
targets. During pivot, a mount inside the current root also takes precedence
over a bootstrap alias outside it for the same visible target. This keeps
the existing bootstrap fallback without resurrecting old mounts. No app
names, fixed mount counts, new syscall emulation, or Flatpak changes.

The new bwrap-socket.py case runs two nested shipped musl bwrap invocations
under the Android syscall filter, binds the mapped X11 socket each time,
and narrows /dev/shm to successive subdirectories. It fails on main with
"Can't open source /dev/shm/inner: No such file or directory" and passes with
the fix, including reading a marker from the innermost shared-memory mount.
The first-level socket regression remains in place.

Device follow-up (coordinator; no VM run here): rebuild the subsystem loader,
fresh launch Kate as a control, then Steam with v3-trace and payload stderr.
Verify the nested DISPLAY probe's /dev/shm bind succeeds; capture its exit,
portal Spawn replies, and subsequent exec-argv lines through steamwebhelper.
The supplied trace cannot establish what Steam does after this failing probe.
No shared-device-tree or kernel changes are required.

Validation: full tests/run.sh passed (including nested socket/shm, pipe
readlink, growing fd tracking, musl Flatpak/ldconfig, i386, and rewrite
checks). Production build-android.sh passed with an isolated output directory;
device-tree preflight and git diff --check passed. Logs:
/mnt/data/aosp/out/pc-logs/agents/steam-r16-{suite,android,preflight}.log.

### Steam round 17: i386 DNS helper startup

Rebased on main. day-r25/steam.txt reaches ubuntu12_32/steam (pid 17111)
at 20:31:35.533, not steamwebhelper. The updater reports manifest download
failure (HTTP status 0), exhausted hosts, and "Steam needs to be online to
update" at 20:31:36.187–.194. Before that, the resolver's i386 socketcall
connects return -EIO twice at 20:31:35.965–.966. Trace operation 29 means
connect, not kernel syscall 29. The runtime first tries the absent systemd
resolver socket, then the configured DNS endpoint.

The proxy's DNS helper startup wrongly classified a valid mmap pointer as
an error whenever its signed value was negative. On i386, ordinary mappings
above 2 GiB have precisely that representation. It now tests Linux's actual
reserved errno range (-4095 through -1), as the VFS allocator already does.
No DNS protocol, resolver selection, Flatpak or application rules changed.

The new freestanding i386-dns-start.S regression uses socketcall to create
and connect a UDP socket to the configured resolver under the Android filter.
This starts the actual proxy worker with a kernel-allocated stack; no external
DNS service or network is needed. It exits 1 with the pre-fix loader and 0
with the fix. This guards startup, not netd's device-side response.

The updater UI subprocess has a separate failure: pid 17114 execs /bin/sh
for steam -child-update-ui, then the supervisor records wait status 31
(SIGSYS). The updater logs ECONNRESET reading its UI socket, then EPIPE and
"error communicating with child process, detaching". The supplied trace has
no child fatal report identifying its syscall. This is not evidence of a CEF
sandbox failure: no steamwebhelper exec occurs. Do not infer that fixing DNS
alone restores the UI.

Coordinator follow-up (no VM run here): rebuild both loader ABIs, freshly
launch Steam, retain payload stderr and v3-trace through the manifest download.
Check that resolver connects no longer return EIO and the update downloads,
then follow exec-argv to steamwebhelper. If the updater UI still dies, capture
the SIGSYS siginfo (si_syscall, si_arch, si_call_addr) for the /bin/sh child
running -child-update-ui, alongside its exec/adoption trace. No kernel or
shared-device-tree changes are required.

Validation: production x86_64/i386 build-android.sh and device preflight
passed. tests/run.sh passed through musl/nested proc-dirfd, then returned
143 (SIGTERM) before the Chrome per-app-dir checks; no assertion failure
was printed. The remaining Chrome per-app-dir, apply-extra, loader-maps and
site-rewrite checks were run separately and all passed, using an isolated
steam-r17-tail directory for the Chrome/apply-extra fixtures. Suite/build
logs: /mnt/data/aosp/out/pc-logs/agents/steam-r17-{suite,android,preflight}.log.
The new regression's before/after logs are steam-r17-{before,after}.log in
the same directory. No VM or shared host-smoke fixture was used.

### Steam round 18: i386 DNS helper stack alignment

Rebased on main. day-r26/steam.txt shows pid 18553 (ubuntu12_32/steam,
-no-cef-sandbox) dying of SIGSEGV at 21:02:00.271, immediately after
`v3 trap ours 295 47` (sendto of the 47-byte resolver query) at .268. The
last trapped calls are 267 rt_sigprocmask, 330 socket (fd 13), 29 connect
-ENOENT (absent systemd resolver), 27 close, 330 socket (fd 13), 317
setsockopt, 29 connect 0 (the configured endpoint, which starts the helper)
and that sendto. No poll/recvfrom follows because poll is executed inside the
trap frame and only logs on return. Operation numbers are the common
PROXY_SC_* enum, not i386 syscall numbers.

The missing crash report is itself the finding: no capture (day-r24 chrome or
obsidian, day-r25 or day-r26 steam, day-r26 kate) contains `v3 fatal`,
`fatal-segv` or `fatal-sigsys` for that pid, so the fault never reached a
proxy handler. Reproduced on the host instead of on a device, with a new
freestanding socketcall test against build/linux-layer-v3-i386: gdb stops in
dns_worker at `movaps %xmm0,-0x88(%ebp)` with si_code 128 (SI_KERNEL) and
ebp=0xf7a22ff4: the destination 0xf7a22f6c is 12 mod 16, four bytes short of
the boundary movaps requires.

Cause: arch/i386/raw.S proxy_raw_clone_thread entered the helper with
esp=stack_top-8, i.e. 8 mod 16, while i386 SysV requires esp%16==12 at
function entry (16-byte aligned at the call site). The build uses -msse2, so
clang emits movaps into a frame slot it believes aligned and the helper takes
#GP the first time the query path initialises an aligned local. Round 17's
mmap errno-range fix is what made this reachable: in day-r25 the resolver
connect returned -EIO because the helper never started, so no helper thread
existed to fault. Nothing about the payload, DNS protocol or netd wire format
is involved.

The death was silent because dns_start blocks every signal across
proxy_raw_clone_thread and the worker inherits that mask. A synchronous
SIGSEGV under a blocked mask is forced to SIG_DFL: no handler, no report, no
tombstone, just the shell's "Segmentation fault". Three changes:

- proxy_raw_clone_thread aligns the child stack (and $-16, sub $12) before
  push arg/call entry, so the helper enters with the ABI's 12 mod 16.
- dns_worker now takes the layer-thread state a payload thread gets on its
  first trap (thread_state: registered tid plus a 1 MiB alternate signal
  stack) and unblocks only SIGSEGV, SIGBUS, SIGFPE and SIGILL. Those are
  synchronous, so they can only come from this thread's own execution and
  cannot steal the payload's signals.
- dispatch_segv_app logs `fatal-layer` with the faulting pc when that pc lies
  outside every payload image, so an i386 SIGSEGV in layer code is reported
  as a MatonOS crash instead of an unexplained app fault. x86_64 app crashes
  still go through crash_report unchanged.

With the alignment reverted and the other two changes kept, the same test now
reports `fatal-segv 128`, `fatal-segv-pc 0x7032...` (dns_worker) and
`fatal-layer` before dying, which is exactly the report the device capture
lacked. The helper's vfs_alloc keeps working from an unregistered stack: it
falls back to vfs_palloc, one page per netd command, freed once the command
is built.

The new regression is tests/i386-dns-query.S: socketcall socket
(SOCK_DGRAM|SOCK_NONBLOCK), connect, sendto 47 bytes, poll 2 s, recvfrom that
must report EAGAIN. It exits 139 with the pre-fix loader and 0 after the fix,
and runs next to i386-dns-start in tests/run.sh. It covers helper survival,
not netd's device-side answer; tests/dns-helper.c still owns the wire round
trip.

This is not the SIGSYS clash. 18553 died of signal 11; the SIGSYS death in
the same window is pid 18556 (/bin/sh -c ... steam -child-update-ui, wait
status 31), which fork-adopt round 24 owns.

Validation: build.sh, build-i386.sh, the production build-android.sh
(x86_64 and i386 GPU-bridge loaders), device preflight (6/6) and the full
tests/run.sh (rc=0, including the new i386 query case, the 64-bit dns-helper/
dns-host/dns-translation cases and the i386 SSE signal-frame cases). No VM was
used.
Follow-up on the next device run: rebuild both ABIs, relaunch Steam and keep
v3-trace through the manifest download. A repeat of this crash would now name
the layer instead of vanishing: `fatal-segv` and `fatal-segv-pc` give the pc
and `fatal-layer` states that it is layer code, with layer image base
0x70000000 on i386 and 0x600000000000 on x86_64.

### Steam round 19: portal survives payload teardowns (2026-10-28, app-side)

Steam's client update completed and the app survived its self-restart, but
the wrapper's portal probe failed again after the restart. The per-stub
upstream `flatpak portal` process ran in the compat-proxy host-command tree,
which dies with the payload tree that hosted it (`hc_server` sets
PDEATHSIG on the first sandbox process; workers and the portal follow).
The session bus (in-process broker) and the Java portal backend were still
alive, but `org.freedesktop.portal.Flatpak` had no owner, so
`flatpak-spawn` / the Steam wrapper check failed for the relaunched payload.
Nothing ever restarted the portal: `HostCommandBackend.startFlatpakPortal`
was start-once and its watcher only nulled the child reference on exit.

Changes (subsystem app, no proxy/AOSP/kernel change):

- `HostCommandBackend` now remembers the portal argv/env and restarts the
  portal whenever it exits, until the runtime is closed. The restarter
  retries at a modest delay while no payload host-command socket can take a
  new portal (spawn fails fast when the socket is absent), so it never spins
  and reacts to whatever payload is currently resident. A guard in the
  re-entrant spawn path prevents any double spawn.
- `JavaPortal.serve()` no longer tears the whole backend down when its one
  broker connection ends: it now logs and re-enters the accept loop until
  the portal is explicitly closed, keeping the backend and the flatpak
  portal process resident for the stub's lifetime. Previously a lost broker
  channel fell into the `finally` and `commands.close()` killed the portal.
- The dbus-review fixture `JavaPortal` double gained the
  `setFlatpakDirectories` method it was missing (pre-existing compile break
  in the dev-only suite since the portal landed).

Regression: `HostCommandBackendTest.portalRestartRegression()` drives two
consecutive portal exits as payload teardowns and asserts a fresh portal
child is spawned each time (three consecutive payload launches all reach
`org.freedesktop.portal.Flatpak`), and that `close()` stops restarts.
Validation: `test-host-command.sh`, `test-portals.sh`, the dbus-review JVM
suite and `make check-inhibit check-portal-wire check-policy-wildcard` all
pass; the touched portal sources compile against the SDK 36 android.jar.
No VM was used; device retest is the coordinator's.

Remaining risk: a brand-new payload can race the restarted portal's
name registration on the bus (a `flatpak-spawn` arriving in the few
milliseconds before `org.freedesktop.portal.Flatpak` is re-owned fails
once). Steam's own repeated probes absorb this; a retry in the caller also
works. The broker still never reconnects a lost Java portal backend channel,
so only the Desktop (Java) portal interfaces would be affected by such a
loss; the Flatpak portal (what Steam's check uses) is served by the separate
restarted process and is unaffected.

### Steam round 20: portal display probe completion (2026-10-28)

The four `Cannot start upstream Flatpak portal: Connection refused` messages
in day-r28 are startup retries against a stale host-command socket, before
`host-command-start` at 22:27:53.426. Startup succeeds at 22:27:53.494.
They are not evidence that the portal remains absent.

At 22:27:57.064 pid 23761 runs the updated client's
`steam-runtime-launch-client --bus-name org.freedesktop.portal.Flatpak
--app-path '' --share-pids -- /bin/sh -euc
'echo "${DISPLAY+DISPLAY_is_set}"'`. The portal starts Flatpak pid 23764,
then nested bwrap, then shell pid 23770 at 22:27:58.241. The shell's write
returns 15 (the length of `DISPLAY_is_set\n`) at 22:27:58.255. Thus this
probe reaches the nested payload; it is not a missing-service failure.
The launch client continues polling until 22:28:07.503, followed by the
wrapper's error dialog. The trace does not show whether SpawnExited was
sent or whether stdout EOF was lost; neither cause is established yet.

A new host-command-pair regression runs that shell through nested stock
bwrap under the proxy and Android filter and checks output, EOF and status
zero. It passes. This covers the resident command path, not the complete
Steam runtime/portal interaction. No per-app workaround was added.

`MatonPortalBus` logcat records Flatpak portal calls, matching replies and
signals (including SpawnExited), since the in-process broker's stderr is
not captured by payload-stderr. The next fresh device capture should retain
this tag, plus the existing exec trace and /proc fd/wchan/status snapshots
of the launch client, portal and nested bwrap after the shell prints. Match
the Spawn reply PID to SpawnExited's PID/status. A missing signal points to
child collection; a delivered signal with a still-polling launch client
requires checking stdout pipe holders. Black-window failure remains open.

Validation: host-command-pair.py, test-host-command.sh and broker
check-inhibit/check-portal-wire/check-policy-wildcard pass. No VM used.
Android broker compilation and linking also pass with SDK NDK
30.0.16248370 and the pinned static dependencies (lib64 prefix exposed via
a private lib symlink view under the test output directory).
