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
### Chromium SIGSYS ownership (fork-adopt round 24)

The resident x86_64 adoption handler forwarded SIGSYS registrations to the
proxy, but sent read-only `rt_sigaction(SIGSYS, NULL, old, 8)` queries to the
kernel. Chromium therefore saw the resident handler instead of the payload's
initial SIG_DFL and refused to start its GPU sandbox. Both query and update
now use the proxy's virtual thread-group disposition. Adopted sigprocmask
calls also use the proxy, which reports the payload's requested SIGSYS mask
while keeping the resident route deliverable; app altstacks remain virtual.
During a payload SIGSYS callback the virtual handler mask blocks SIGSYS as
Linux requires, but the real mask explicitly unblocks it: the adoption
handler entered with real SIGSYS blocked, and callback syscalls must still
re-enter the proxy. The interrupted ucontext retains the pre-handler mask.

The retained payload filter evaluator now selects the highest-precedence
stacked action (newest wins ties) and delivers RET_TRAP against the interrupted
payload ucontext, with SYS_SECCOMP, syscall, architecture, RET_DATA errno and
call address. Handler register edits survive the proxy epilogue. The kernel
copy of each payload filter exempts the exact resident raw syscall return PC:
proxy bookkeeping and trace getpid calls must never invoke the payload trap
handler. Rewritten payload calls evaluate the original filter before entering
that exempt gateway; TRAP uses the dedicated payload syscall gateway instead.
The direct seccomp syscall is now routed through this same installation
path (it previously bypassed the dispatcher despite having a case there).
No app names or Chromium-specific policy are involved.

`tests/sigsys-virtual.c` checks initial/default and installed dispositions,
virtual mask/altstack queries (including the handler mask and interrupted
ucontext mask), RET_TRAP siginfo and RAX edits for getpid and a
warmed libc openat path, both prctl and seccomp filter-installation APIs,
and RET_ERRNO data-zero equivalence, in a parent
and adopted fork child. `tests/run.sh`
runs it through both static and resident-adoption launchers, including trace
mode. The old main adoption handler reproduces the initial-disposition failure
in a host comparison. Validation logs on this machine:
`/mnt/data/aosp/out/pc-logs/agents/fork-adopt-r24-{suite,android,baseline}.log`.
The native test also passes without the proxy; in particular RET_ERRNO data
zero skips the syscall and returns zero, rather than manufacturing EPERM.

Device validation remains pending (no VM started for this task). Rebuild the
subsystem and fresh-boot the coordinator's image, then launch stock Obsidian
and Chrome with v3-trace for 90 seconds each. Capture full logcat and a window
snapshot: check that the "Existing signal handler" diagnostic disappears,
GPU/renderer processes stay alive, and each browser creates a toplevel. Keep
Kate as the working control. If a browser still fails, retain its first GPU
failure and preceding syscall/signal trace, rather than just the final GPU
manager fatal. No shared-device-tree or kernel changes are required.

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

### Chromium SIGSYS default disposition after exec (fork-adopt round 25)

The disposition is already process-wide (`chained`); a worker install is visible
from the main thread and other workers. The remaining Chromium rejection has a
separate reproducible cause: resident exec reset only `chained.handler`, retaining
`SA_SIGINFO` from the previous payload's caught action. Chromium's
`IsDefaultSignalAction` in `sandbox/linux/seccomp-bpf/trap.cc` rejects either a non-default pointer **or**
`SA_SIGINFO`, so a null pointer alone did not restore the default disposition.
Reset the complete caught SIGSYS action on successful resident exec; preserve
SIG_IGN as Linux does. Masks remain per-thread.

`tests/sigsys-virtual.c` now installs a caught action, re-execs `/proc/self/exe`,
and creates a worker which queries the default disposition (including flags),
installs its handler and a TSYNC seccomp TRAP filter, and handles a trap. The main
thread and a subsequent worker both observe that handler and handle traps with
register edits. This passes natively and failed on the previous proxy at the
worker's default query, under both static and resident-adoption launchers.
The regular suite also runs the adoption case with tracing enabled.

Device verification still required: freshly build the loader and rerun Chrome
and Obsidian with v3-trace for 90 seconds, retaining exec argv, stderr, compositor
xdg_toplevel events and a screenshot. Check that Chrome's `Existing signal
handler when trying to install SIGSYS` and GPU exit loop disappear, Chrome keeps
its toplevel, and Obsidian still renders its vault picker. No VM was run here.

Round-25 host verification: full `bash linux/compat-proxy/tests/run.sh` passed
(including i386 suites, both traced SIGSYS launchers, apply-extra and rewrite
regressions); standalone Android x86_64/i386 loader builds and device-tree
preflight passed. Logs: `/mnt/data/aosp/out/pc-logs/fork-adopt-round25-{tests,android,preflight}.log`.
### Steam round 21: display probe completes; client architecture assertion

The day-r29 capture disproves a stuck portal completion in this run:
`Spawn` at 23:31:36.347 receives PID 26695 at 23:31:36.378;
`SpawnExited(26695, 0)` is routed to the live client at 23:31:37.611.
Steam then execs `ubuntu12_32/steam` as PID 26701 at 23:31:37.627.
Do not fix portal routing based on the previous run's timeout alone.

The next failures are separate: the update UI child (26706) starts an
exec from i386 to the runtime's x86_64 `/bin/sh` at 23:31:40.788, then
its parent reports `ProcessMessagesFromChild: failed to read header`
and `CUpdateUILinux: error communicating with child process, detaching: 32`.
Installation verification completes at 23:31:55.593. At 23:32:10.276 the
client reports `../common/friendsuihelpers.cpp (54) : Is64BitOS()`.
No steamwebhelper exec occurs. These observations locate failures but do
not establish the client architecture check's implementation or the UI
child's exit cause.

Bionic's `libc_init_common.cpp::__initialize_personality` sets PER_LINUX32
on ELF32 startup, including our resident i386 loader; the exec state stream
does not transfer the caller's persona. This is a candidate explanation,
not proof that Steam's assertion reads uname. Opt-in `v3 uname` diagnostics
now report the actual kernel machine answer, kernel/tracked persona, ABI,
PID and result without changing them. Peer exec/import breadcrumbs show
whether the UI child's cross-ABI loader handoff completes. The uname host
regression compares native Linux output for PER_LINUX and PER_LINUX32 and
checks invalid-pointer EFAULT under tracing. Both production Android loader
ABIs are compiled as part of validation.

Next device capture: same fresh launch with v3-trace and payload-stderr;
include console-linux.txt and Steam's bootstrap/client logs plus the app UID
process tree with wchan/syscall/fd targets after the Is64BitOS assertion.
Retain `v3 uname`, `exec-peer-enter`, `exec-peer-return`, `exec-state-import`
and fatal/lifecycle lines. This will distinguish a persona-induced machine
answer from a failed cross-ABI exec and identify the remaining wait.
No VM, kernel change, AOSP patch or per-app workaround is part of this round.

Validation: full proxy suite (private external fixture roots), nested
host-command-pair display probe, native uname comparison/EFAULT, both
production Android loader builds, device-tree preflight and diff checks pass.
Device confirmation remains pending with the coordinator; no Java changed.

### Chrome PCI enumeration (fork-adopt round 26)

The day-r30 Chrome trace is not silent: PIDs 29107 and 29129 end their
traced operations by opening `/sys/bus/pci/devices`, then writing
`pcilib: Cannot open /sys/bus/pci/devices`. The browser subsequently reports
GPU exit_code=256. The generated GPU tree had `/bus/pci` but no `devices`
subdirectory; its DRM device and PCI attributes already existed elsewhere
in the same tree. Publish the standard bus directory and its device symlink
to that existing synthetic device. No host PCI access, app-specific flags,
GL selection changes or sandbox exceptions are needed.

Obsidian day-r29 also has this diagnostic (PID 26086), despite rendering;
its working UI is not evidence that PCI enumeration succeeded. Chrome uses
`--ozone-platform=wayland`, while Obsidian starts with
`--ozone-platform-hint=auto` and its captured child uses X11. The captures
do not expose GPU fork argv sufficiently to establish a GL/ANGLE difference.
Repair the demonstrated missing sysfs interface rather than changing those
choices. Device confirmation of the GPU exit loop remains necessary.

`tests/gpu-sysfs.c` now enumerates the bus with opendir/readdir and opens its
entry relative to the directory fd, checks identity attributes and verifies
that the bus and DRM aliases identify the same inode. This failed before the
fix with ENOENT. `tests/gpu-host.sh` additionally runs real `lspci -n` when
installed, and checks the same fixture after bwrap root binding/pivot. GPU,
no-device, software and invalid-property cases pass. Android x86_64/i386
loader builds pass. Logs are under
`/mnt/data/aosp/out/pc-logs/fork-adopt-round26-{gpu,android}.log`.

Coordinator: rebuild the loader/APK and capture Chrome and Obsidian with
v3-trace for 90 seconds on the fresh image. Check that PCI enumeration no
longer prints the pcilib error, Chrome retains its toplevel without the GPU
exit loop, and Obsidian still renders. Preserve stderr, exec argv, compositor
messages and screenshots. No VM was run; no kernel, AOSP or shared device-tree
changes are required.

Round-26 final verification: full `bash linux/compat-proxy/tests/run.sh`
passed, including i386, mixed exec, apply-extra, GPU/bwrap, SIGSYS and rewrite
checks. The preexisting zygote stream test now requests precisely each fixed
message length with MSG_WAITALL; an oversized read had intermittently consumed
the next message's first byte. Its corrected form passed ten consecutive runs.
The 10,000-thread stress passed with a 60-second deadline after the shared
host exceeded its previous 15-second deadline (standalone retry passed within
30 seconds). No worker count was reduced. Android app Java compilation,
MachineIdTest and device-tree preflight also passed. Logs:
`/mnt/data/aosp/out/pc-logs/fork-adopt-round26-{tests,java,preflight,baseline}.log`.
### Steam round 22: preserve the caller's persona across resident ABI exec

The day-r30 display probe completes and Steam enters the i386 client, PID
29800 (resident import at 23:58:28.660). Its first uname answers, and all
later answers around the Is64BitOS assertions, report kernel persona 8,
`machine=i686`. Bionic's `__initialize_personality` unconditionally selects
PER_LINUX32 for ELF32; our cross-ABI exec stream omitted personality. The
Linux payload thus inherited an implementation detail of its resident loader.
This proves the incorrect machine view; the proprietary Is64BitOS function's
exact implementation is not established by syscall tracing alone.

The private exec stream now transfers the calling thread's persona (wire
version 9) and restores it after bionic startup, retaining virtual state when
Android refuses flags. Fresh Android ELF32 loader launches clear bionic's
base-persona selection to PER_LINUX, retaining the other flags. Explicit
payload PER_LINUX32 requests remain valid and survive exec; uname is not
hardcoded to x86_64. No Steam-specific rule is involved.

The mixed-class regression now transfers a virtual ADDR_NO_RANDOMIZE plus
PER_LINUX32 through both ABI changes, checks query semantics, switches back
to PER_LINUX and verifies uname reports x86_64. Before this change the virtual
flags disappear on the first resident exec. Both production Android loader
ABIs are compiled. Device validation remains necessary: rerun Steam traced,
retain uname/personality and exec-import lines, stderr and bootstrap/client
logs. PID ubuntu12_32/steam should report persona 0 and machine=x86_64 until
it explicitly changes persona; check whether Is64BitOS disappears and whether
steamwebhelper executes. No VM was run; no shared, kernel or AOSP change.

Validation: full `bash linux/compat-proxy/tests/run.sh` passed, including
mixed-class exec in both directions and bwrap, uname/EFAULT, personality,
i386, nested launch and rewrite regressions. Both production Android loader
builds passed. Evidence: `/mnt/data/aosp/out/pc-logs/steam-r22-tests.log`,
`steam-r22-cross.log`, `steam-r22-android.log`.

### Steam round 23: localize the missing webhelper handoff

The day-r31 capture does not yet demonstrate a CEF executable starting.
PID 31913 logs `sh -c exec .../ubuntu12_64/steamwebhelper.sh ...` at
00:33:40.173, validates its ELF64 shell, then logs `exec-peer-enter
1073741827 8` at 00:33:40.179. There is no peer-return, state-import,
subsequent exec-argv, or fatal report for that PID. Other ELF32-to-ELF64
children (31836, 31908, 31910, 31912, 31935 and later) also stop at
peer-enter; the last successful imported resident is ELF32 PID 31833.
Steam remains alive, with nonblocking socket reads returning EAGAIN. The
libudev netlink permission errors are not evidence of a CEF graphics failure.

Import previously restored its diagnostic fd only in proxy_state, after
VFS/IPC/network import. A failure earlier silently returned status 2, while
payload stderr could point at Steam's steamui_html.txt. The private wire
format (version 10) now restores trace/sink immediately after authenticated
header validation and logs entry plus the failing import section/cursor.
This fixes observability, not a demonstrated client compatibility fault.
The malformed-body regression uses a valid sealed header and redirects fd 2
to /dev/null; both ABIs must report the VFS failure through the inherited
sink. Existing mixed-class round trips exercise the updated stream.

Coordinator next capture: run Steam traced on a fresh image with both new
loaders. Preserve exec-state-enter, exec-state-*-error, exec-state-import,
exec-peer-return and stderr. Around 110 seconds capture every Steam-UID
process's cmdline, status, wchan, syscall and fd targets, plus Steam logs
steamui_html.txt, webhelper-linux.txt and cef_log.txt if present. In
particular follow the pid whose argv names steamwebhelper.sh: if no import
entry appears, inspect whether it remains in the Android linker, is dead,
or has rejected the header; if an import error appears, its section and
cursor identify the failing state. Do not infer a SIGSYS clash or shared
Ozone/dma-buf failure without a CEF exec or matching diagnostic. No VM,
per-app behavior, shared device-tree changes, kernel changes or AOSP patch.

Validation: full proxy suite passed; mixed-class exec and the redirected
stderr malformed-state regression passed for both ABIs. Production Android
x86_64/i386 loader builds and device-tree preflight passed. Evidence:
`/mnt/data/aosp/out/pc-logs/steam-r23-{tests,cross,android,preflight}.log`.
Device compatibility remains unverified; this round supplies diagnostics.

### Steam round 24: separate resident-linker and payload environments

In day-r32, ELF32 PID 3130 requests the ELF64 shell with `exec
.../ubuntu12_64/steamwebhelper.sh ...` at 01:32:01.044, reaches
`exec-peer-enter 1073741827 8` at 01:32:01.052, and never reports
`exec-state-enter`, import failure, or peer-return. PID 3055 and the later
ELF32-to-ELF64 children stop at the same boundary. The successful imports
are in the other direction. This capture still does not prove a CEF binary
has started; it localizes the missing transition before authenticated import.

The handoff passed the Linux payload environment directly to kernel exec
of the Android resident. Consequently bionic's linker consumed payload
`LD_LIBRARY_PATH`/`LD_PRELOAD` before main, before VFS import, and with native
path semantics. That is an independent generic startup bug: those settings
belong to the Linux linker, and can name incompatible libraries or virtual
paths. Wire version 11 preserves the exact payload environment (including
order and duplicate keys) in sealed state. Native resident startup excludes
`LD_*`; main restores the saved environment for proxy_load and the Linux
linker. No payload environment setting is dropped, and no app is special-cased.
`exec-native-env <total> <excluded>` records the boundary without logging values.
Both resident ABIs must be updated together.

The new cross-exec regression inserts a native peer that inspects the kernel
startup environment before importing state, then execs the real resident.
It uses the fixed raw gateway because host filters cannot start an arbitrary
native dynamic linker. Both payload ABIs check the restored LD_LIBRARY_PATH
and LD_BIND_NOW after exec. An isolated negative control with only native
filtering disabled fails with `native resident received payload LD_*`; the
fixed round trip passes. Ordinary static-loader round trips alone hid this
boundary. Header rejection tests now use the current version when testing
other header fields.

This is a tested environment-boundary fix, not device confirmation that it
caused Steam's missing handoff: day-r32 has no redirected linker stderr or
process snapshot. Coordinator: rerun Steam traced with both rebuilt loaders;
follow the steamwebhelper.sh shell PID through exec-native-env,
exec-state-enter/import and subsequent exec-argv. Capture Steam's
steamui_html.txt, webhelper-linux.txt, cef_log.txt and console-linux.txt,
plus cmdline/status/wchan/syscall/fds for that PID if import remains absent.
Those logs are essential because native linker stderr can be redirected by
Steam. No VM was run; no kernel, AOSP, device-tree or per-app change.

Validation: full `bash linux/compat-proxy/tests/run.sh` passed, including
native-startup environment regression, ABI round trips, bwrap, malformed
state, production Android x86_64/i386 compilation and packaging, EROFS,
ldconfig and rewrite tests. Negative control, device-tree preflight and
`git diff --check` passed. Evidence:
`/mnt/data/aosp/out/pc-logs/steam-r24-{tests,cross,android,negative,preflight}.log`.

### Steam round 25: the peer exec barrier is the inherited native filter

Day-r33 (180 s v3-trace, VM main b9bd3d8 including the round-24 environment
fix) still never starts a CEF process. Steam's i386 client forks each
webhelper, and every i386-to-x86_64 exec dies at the same boundary as
rounds 23-24: `exec-peer-enter 1073741827 <fd>` is logged, then nothing —
no `exec-state-enter`, no import rejection, no `exec-peer-return`, no fatal
report, no Android crash. The webhelper PIDs (5980, 6027, 6033, 6039, 6045,
6051, 6057) each log exactly `sh`, `-c` and the `steamwebhelper.sh` exec and
then vanish; Steam restarts them every 10 s (its webhelper log shows
"Restart webhelper process, counter 2"). `gldriverquery`, `vulkandriverquery`,
`steamsysinfo`, `lspci` and `steam-runtime-launcher-service` (all x86_64,
spawned from the i386 client) stop at the same line. The capture contains 8
peer-enters from i386 with **zero** imports, and 5 peer-enters from x86_64
(the steam client and its `steamui` verify helpers) with 5 successful i386
imports. Chrome and Obsidian render because they are native x86_64 and never
cross ABIs; the Steam client is the first process to hand an exec to the
x86_64 peer.

Root cause. The i386 client is a fresh resident created by a kernel exec
from the x86_64 launcher chain (flatpak → bwrap → bash all run in one loader
process). Seccomp filters survive kernel exec, so the launcher's **native
x86_64 payload filter** (`arch_filter_install`, whose number-only fast path
still TRAPs every `ours()` syscall: mmap, read, openat, pread64, fstat,
close, ioctl, ...) is in every descendant's chain. When the i386 client
kernel-execs the x86_64 peer loader, the fresh resident starts its **bionic
startup under that inherited native filter**. Its very first mmap is TRAP'd,
and kernel exec discarded the SIGSYS handler, so the process dies silently —
no handler, no tombstone, no proxy report. The reverse direction works
because the launcher filter's i386 branch is the *peer* branch, which
`MATON_GPU_BRIDGE` builds already exempt with a blanket ALLOW
("Let that ABI start under Android's inherited app filter"). The peer exec
from i386 is thus the only path that runs the x86_64 payload under a native
x86_64 proxy filter before the resident installs its own routing. Earlier
evidence already pointed here: round 17's update-UI child (`/bin/sh` exec
from the i386 client) died with wait status 31 (SIGSYS), and round 21
recorded the same boundary.

Fix. A freestanding ELF64 peer loader (`libmatonos-v3-loader-static.so`,
built in `build-android.sh` with the same flags as the host `linux-layer-v3`:
`-nostdlib -static -no-pie --image-base=0x600000000000` plus the
`.proxy_gateway` section at 0x600000300000). It starts at its own `_start`,
so there is no bionic startup to run under the inherited native filter: every
syscall before `proxy_install` is a gateway-exempt raw call, the inherited
TRAPs never fire, and it installs its own SIGSYS handler and filter before
loading the payload. The i386 backend's `ARCH_PEER_LOADER` now names this
static loader; the bionic x86_64 loader remains the v3 launch path where GPU
thread adoption needs it. Peer-exec'd x86_64 payloads run without the bionic
GPU bridge, which those execs never requested; their GL is a separate concern
for the next round. `arch_app_install` additionally exempts the peer ABI
branch with the same blanket ALLOW the payload filter already uses, so a peer
ABI's bionic startup can never hit that filter's ioctl/fd/non-allowlist TRAPs
either.

Diagnostics. A resident that died before import was completely silent: the
fresh image has a zero trace flag and no imported sink. Resident entry now
logs `resident-enter <argc> <PR_GET_SECCOMP>` on the inherited fd 2 (the
app-side relay), and every pre-header import check reports its stage
(`resident-import-fd/seals/size/mmap/magic/version/pid/nonce`) instead of
returning status 2 silently. `exec-state-import` is also logged on failure
even when the payload disabled tracing. This distinguishes a kernel-to-bionic
startup death (no `resident-enter`) from a rejected handoff (stage named).

Device follow-up: rebuild the APK (all three loaders), fresh-boot, launch
Steam with v3-trace, and follow the webhelper PID past `exec-peer-enter`:
expect `resident-enter`, `exec-state-enter/import`, then exec-argv for the
CEF binary and its `--type=` children. If the webhelper still restarts, the
next failure is after the exec barrier (GL/display), not the handoff. Keep
Chrome/Obsidian as controls. No VM was run; the host suite (below) passed
with the new `arch_app_install` peer branch and diagnostics.

Validation: full `bash linux/compat-proxy/tests/run.sh` passed, including the
redirected-stderr early-import regressions for both ABIs, the mixed-class exec
round trips and the three-loader packaging check; `tests/state-reject.py`
covers `resident-enter` plus each rejection stage. Host evidence:
`/tmp/opencode/r25-tests-final.log` (pre-rebase) and
`/tmp/opencode/r25-tests-rebased.log` (post-rebase, exit 0). The Android
`build-android.sh` ran inside the suite and produced all three loaders,
including the new freestanding peer (ELF64 EXEC at 0x600000000000,
`.proxy_gateway` at 0x600000300000); the coordinator's device build still
validates the packaged result.


### Steam round 26: nested display probe writes, completion remains unknown

Rebased steam-personality on main (bc2c3d3, already up to date). In day-r34,
the portal accepts the display probe and returns PID 11140 at 04:29:38.175.
Its nested bwrap forks PID 11144, which starts `/bin/sh -euc
'echo "${DISPLAY+DISPLAY_is_set}"'` as PID 11146. The shell writes 15 bytes
successfully at 04:29:39.423. At 04:29:48.487 the caller sends
SpawnSignal(11140, SIGTERM, false); the portal routes SpawnExited(11140, 15)
at 04:29:48.496. Thus the bus, Spawn reply and exit-signal forwarding work;
this is a timeout after the shell writes, not a missing portal or evidence
that the peer loader lost DBUS_SESSION_BUS_ADDRESS. It is not yet established
whether the shell exits, the bwrap init/monitor fails to finish, or a pipe
writer prevents the launch client from observing EOF. No functional fix is
claimed from these traces.

The host-command regression now includes nested stock bwrap --unshare-pid
(the init/reaper plus outer monitor), checking output, EOF, zero status,
worker reaping and successful status transmission. This passes on the host;
the host resident-adoption launcher also completes this nested probe.
Trace-only diagnostics identify each host-command worker/child pair, reaped
PID/raw wait status and status-send result, and adopted exit_group PID/code.
No per-app behavior, portal routing or wait semantics change.

Next coordinator capture: launch Steam with v3-trace and collect the usual
log plus the Steam UID process tree at about 6 seconds, before the ten-second
probe timeout. For each process retain argv, PPid, State, SigBlk, SigPnd,
ShdPnd, wchan, syscall and every fd target plus fdinfo flags. Match the
stdout pipe inode of the display-probe shell and report all holders with
write access. Retain host-command-child/reap/status, adopt-exit-group,
lifecycle-exit-group, SpawnSignal and SpawnExited. This distinguishes an
alive shell from bwrap wait/monitor blockage and a leaked stdout writer in
one capture. No VM was used; Android/device confirmation remains pending.

Validation: full `bash linux/compat-proxy/tests/run.sh` exits zero, including
all three Android loader builds, mixed-ABI exec/import, traced adoption,
rewrites and the strengthened host-command-pair regression. The adopted
SIGSYS regression logs adopt-exit-group for parent and children. Evidence:
`/mnt/data/aosp/out/pc-logs/agents/steam-r26-suite.log` and
`steam-r26-pair.log`. No Java or image app code changed.

### Steam round 27: completed shell, stuck bwrap signalfd monitor

Rebased on main 61311e6. day-r35 proves that the display-probe shell
(12887) writes 15 bytes and calls exit_group(0) at 04:51:53.538; bwrap's
inner reaper (12886) calls exit_group(0) at .542. The outer bwrap monitor
(12881) last created signalfd4 fd 3 and restarted infinite poll as ppoll at
04:51:52.738. It never resumes before SpawnSignal(SIGTERM) at 04:52:02.489.
The portal sends and routes SpawnExited(12881, 15) at .498. The host-command
worker only reaps adopted 12886 at .549, after the monitor dies. This rules
out the shell remaining alive and missing portal exit forwarding. It does
not establish why the monitor fails to receive its child's SIGCHLD.

No functional fix is claimed. Host reproduction with both stock glibc bwrap
and the bundled static musl bwrap completes under resident adoption. New
signalfd-child coverage deliberately omits the --unshare-pid eventfd escape
path: twenty immediate/delayed child exits must wake raw infinite poll,
read the matching signalfd notification and reap the matching status. It
passes natively, under the Android-filter freestanding proxy and under
resident adoption. Opt-in poll traces now record kernel/return signal masks,
pending signals, SIGCHLD disposition and each polled fd; adoption logs
SIGCHLD disposition updates. These diagnostics do not consume notifications
or reap children.

Next coordinator capture: run traced and capture the nested probe's bwrap
monitor **5–8 seconds after launch, before the ten-second probe timeout**.
Retain the new poll-signal-state, poll-descriptor and adopt-chld-action lines.
For the monitor and every task in /proc/PID/task, retain status (especially
SigBlk/SigPnd/ShdPnd), syscall, wchan and children; retain monitor fd targets
and fdinfo for all polled fds (signalfd's sigmask in particular). Also retain
the inner reaper's status/PPid. This distinguishes an incorrect restored
mask, an unblocked resident thread consuming SIGCHLD, a wrong signalfd mask,
and a reparented child. Do not infer one of these causes from the present
capture. No VM, image build, app-specific workaround or kernel change.

Validation: full `bash linux/compat-proxy/tests/run.sh` exits zero, including
Android loader compilation/packaging, both ABI suites, host-command-pair,
stock nested bwrap and staged static musl bwrap tests. Evidence:
`/mnt/data/aosp/out/pc-logs/agents/steam-r27-suite.log`. The added signalfd
coverage passes on the unchanged behavior; it is not a reproduction of the
device failure and does not justify changing wait/signal semantics.

### Steam round 28: resident workers can steal signalfd SIGCHLD

Rebased `steam-personality` on main. The existing bwrap-style signalfd test
also passes through `linux-layer-v3 --supervise --android-filter`: the
continuous supervisor reaper does not consume children of a living payload
parent. Its waitid(P_ALL) only covers its own direct/adopted children.

A host fixture with a resident pthread, created before the payload blocks
SIGCHLD and retained across the emulated exec, reproduces notification loss.
Before the fix, the fixture failed at signalfd read with EAGAIN after poll
reported readiness. Linux can deliver process-directed SIGCHLD to the
unblocked resident worker and discard it under SIG_DFL; bwrap then polls
again forever even though its child has exited. The wait status remains
available, but bwrap relies on signalfd to decide when to reap.

The loader now blocks SIGCHLD before native EGL initialization, so its
EGL/Binder workers inherit that blocked bit, and restores the launch
thread's exact previous mask after initialization. Payload signal masks,
signalfd descriptors, dispositions and wait statuses retain kernel Linux
semantics. No synthetic SIGCHLD, status stealing, per-app rule, polling
interval or change to the continuous supervisor reaper is needed.

`tests/resident-signalfd-launch.c` retains a resident worker while the real
adoption/proxy route launches the bwrap-style signalfd test. A continuously
waiting subreaper also reaps a separate child while the monitored payload
forks 20 children, reads the matching SIGCHLD notifications and obtains each
original waitpid status. The suite additionally runs the same monitor under
the production `--supervise` path. The unprotected resident-worker fixture
fails; the protected fixture passes with the Android syscall filter active.

No VM was run. Coordinator follow-up: rebuild both loader ABIs and launch
traced Steam; confirm the display-probe bwrap exits 0 and SpawnExited is
forwarded without its 10-second timeout, then follow steamwebhelper. This
host reproduction identifies a real notification-loss bug; the supplied
capture does not establish the device workers' masks. If the timeout
persists, retain `/proc/<bwrap>/task/*/status` and its signalfd fdinfo while
blocked. Workers created later by native code that explicitly changes its
mask would require separate evidence/handling.

Validation: full `bash linux/compat-proxy/tests/run.sh` exited 0, including
both ABI suites, Android loader compilation/packaging, stock nested bwrap,
and the new resident-worker/supervisor signalfd cases. Full evidence:
`/mnt/data/aosp/out/pc-logs/agents/steam-r28-suite-fixed.log`.

### Steam round 29: downloaded runtime hard links in HOME

Rebased on main 348382c. The supplied day-r36 capture does **not** show nine
CEF restarts: steamui_html.txt has old restart records at 02:31, followed by
one current start at 06:29:30 (PID 17028). In steam.txt this PID execs
steamwebhelper.sh, then the steamrt64 entry point/run/pressure-vessel wrapper.
It is still preparing that runtime at the end of the capture; no actual CEF
steamwebhelper executable or --type= child is reached. The early
lifecycle-exit 49/0 is a thread exit, not exit_group or evidence that the
webhelper parent died. There is no captured CEF exit status/IPC failure to fix.

The trace proves an earlier platform gap: PID 17028 gets 5,960 linkat
(PROXY_SC 151) EACCES results; stderr contains 6,000 hard-link warnings and
copy fallbacks, continuing until 06:30:54. Downloaded runtime source and
preparation destination are both under the stub HOME, outside existing
installation link-store roots. Android denies the real hard links there.
The launcher now establishes the existing .v3-store-root marker in HOME,
using the same persistent inode-store implementation as deployment trees.
No Steam setting, CEF switch or per-app rule is added. Existing homes acquire
the marker before launch; repeated launches preserve it and the backing store.

Regression: tests/home-links.sh compiles the actual Java setup helper, proves
that the runtime-shaped linkat operation fails without its marker under a
kernel EACCES filter, then passes with the marker. It checks shared inode,
link counts, writes through an open source fd, surviving destination after
source unlink, and idempotent setup. Included in tests/run.sh.

Device follow-up (coordinator only): rebuild the host APK, fresh launch with
v3-trace, retain steamui_html.txt with timestamps, and verify runtime linkat
EACCES/copy fallbacks disappear. Allow runtime preparation to finish and then
capture actual steamwebhelper binary exec, --type= children, stderr and any
exit_group/wait status. This fixes the observed preparation gap; a rendering
fix or the old restart loop's cause is not claimed. No VM was started.

Validation: full `bash tests/run.sh` passed (including Android NDK packaging,
i386/cross-exec, hard-link stores, signalfd, bwrap and site-rewrite checks).
Focused home-links test passed again after adding the failing-before assertion.

### Chrome HOME store and detached payload lifetime (Round 30)

`tests/home-links.sh` now pairs the persistent HOME hard-link marker with
Chrome-shaped profile operations: `SingletonLock`, `SingletonSocket` and
`SingletonCookie` symlinks coexist with linked Crashpad data across create,
unlink, rename and read. When `google-chrome` is installed, it also launches
the real host Chrome through the proxy under a kernel filter that denies raw
`linkat`, and checks the headless document output. This host run passed with
Chrome 156.0.8078.47; it did not reproduce the device's blank window, so the
device-side cause remains open pending a trace of the failing HOME syscall.

`tests/supervise-child.c` and `tests/supervise.py` cover the session boundary
directly: a parent exits successfully while a detached child holds a
`window-open` sentinel, and the supervisor must remain alive until that child
exits. Both x86_64 and i386 loader checks pass. The Android stub's
`V3Launcher` waits on this supervisor process before ending the task/session.
`:app:compileDebugJavaWithJavac -x :app:preBuild` passed against SDK 36
(native packaging tasks excluded; no APK or device test claimed).

### Steam round 34: route SIGCHLD without retiring resident threads

Based on main `dd37f21`. The new `tests/exec-signalfd.c` reproduces the
threaded resident-exec failure on that revision: the monitor times out
(exit 124) because an unblocked pre-exec sibling consumes SIGCHLD. The
routing implementation leaves every thread alive and does not change
waitpid/wait4/waitid or the supervisor's adopted-child reaping.

`child-signals.h` retains the payload's SIGCHLD disposition while installing
a resident delivery gate. A notification received on the wrong thread is
sent with `rt_tgsigqueueinfo` to an active sigwait waiter, an open signalfd's
owner, or the current image's main thread. Clone publication records each
payload thread's image generation before releasing its startup gate, so a
late-starting pre-exec sibling cannot register as a current-image waiter.
Signalfd dup/close/CLOEXEC and reads/polls update ownership. Kernel-created
signalfds remain real descriptors. Forwarded notifications use a private
SI_QUEUE envelope; read/readv, sigwaitinfo and ordinary handlers receive
the original PID, UID, CLD code, exit status and CPU times. One outstanding
forward per target preserves standard-signal coalescing. No wait status is
consumed and no scheduling sleep, retry loop or timeout is introduced.
Ignored dispositions and SA_NOCLDWAIT retain kernel auto-reaping semantics.

The resident public pthread_create wrapper extends the existing EGL warmup
mask rule to lazy native workers on both ABIs: they inherit blocked SIGCHLD.
Adopted payload threads restore the caller's payload mask before entering
Linux code and block SIGCHLD again when returning to resident cleanup.
The wrapper does not mark or retire threads. The clone registry, graphics
workers and compositor connections keep their existing lifetimes.

The change exposed compiler inlining of the two 4 KiB /proc/maps buffers
into the long-lived SIGSYS frame. With the real 0x10d30 handler stack this
broke the hard-link fallback's subsequent lazy symbol resolution. Keeping
that parser out of line fixes the stack regression; the existing corrupt
link-count and bwrap pivot-root handler-stack tests exercise it.

Required host coverage in `tests/run.sh`: leader/nonleader threaded exec;
signalfd notification metadata plus untouched wait status; sigwaitinfo;
split readv buffers and fd aliases; a secondary-thread signalfd waiter;
ordinary handlers; SIG_IGN/SA_NOCLDWAIT; real host Flatpak 26.08 runtime →
bwrap → true with a continuously reaping supervisor and a lazy resident
worker (including inherited raw-fork identity); and a real Wayland socket
roundtrip across threaded payload exec with the resident server still alive.
The stock nonblocking signalfd fixture handles advisory poll readiness and
EAGAIN as upstream bwrap does. Tests have external watchdogs; production
routing has none. No VM was started.

Coordinator follow-up: build the updated loaders into the subsystem APK,
then freshly launch Kate, Obsidian and Chrome and verify they draw. Launch
Steam with tracing, verify its portal probe completes without timeout, and
retain any `chld-route` failures, steamwebhelper exec/exit records and
steamui_html.txt. Host routing tests do not establish that Steam renders.
No shared device-tree or kernel changes are needed.

Validation: full `bash tests/run.sh` passed with task-specific TMPDIR,
MATON_CHROME_TEST_OUT and MATON_APPLY_EXTRA_TEST_OUT (exit 0), including the
NDK x86_64 resident, i386 resident and static peer loader builds and packaging
checks. Log: `/mnt/data/aosp/out/pc-logs/agents/steam-r34-suite-final.log`.
The unchanged-main adoption launcher times out on the new threaded-exec
fixture (exit 124), recorded in `steam-r34-baseline-regression.log`; the
routing build completes it. A separate Android build also passed. No APK
was installed and no device rendering result is claimed.

### Steam pressure-vessel proc-dirfd reopen (2026-10-28, round 35)

The day-r42 trace reaches the webhelper shell wrappers, but PID 32586 is
still pressure-vessel preparing its mutable runtime when it crashes. At
12:32:43.907 it reopens O_PATH directory fd 39 (`usr/lib/x86_64-linux-gnu`)
through `/proc/self/fd/39` as readable fd 40, then closes the original. We
recorded that temporary proc alias as fd 40's logical name. Subsequent
`readlinkat(40, "libicutu.so.67", ...)` and all other library symlink reads
failed during path resolution. Linux's reopened descriptor remains attached
to the directory after closing/reusing fd 39.

At 12:32:44.765 the process faults at bundled GLib offset 0x3ddb0:
`g_str_hash`, instruction `movzbl (%rdi),%edx`, with a null argument. The
upstream pressure-vessel `pv_runtime_remove_overridden_libraries` second
pass passes the failed `glnx_readlinkat_malloc` result to its string hash
lookup without checking NULL. PID 32697 repeats the same fault. No actual
CEF binary exec, CEF child, or client/webhelper IPC handshake is reached
in either lifetime; the client waits for initialization and restarts the
wrapper. The host reproduction fails with ENOENT before the VFS fix.

`vfs_fd_record` now snapshots the source descriptor's logical name when
recording a reopened proc-fd alias. No application rule or upstream patch
is needed. `tests/reopen-dirfd.c` exercises O_PATH → proc reopen → source
close → readlinkat/lstat, then source-number reuse, both directly and through
a virtual directory map under the app seccomp filter. Re-run a fresh traced
Steam launch on the coordinator image: the relative reads on the reopened
fd should resolve, both GLib crashes should disappear, and the next capture
should show the actual `steamwebhelper` ELF and any CEF children. Rendering
and the later IPC handshake still require device verification.

Validation: full compat-proxy suite and repository host suite passed; Android
x86_64 resident/static and i386 loader builds passed; device-tree preflight
passed. The suite includes the real Flatpak/bwrap launch with resident workers,
SIGCHLD routing, Wayland roundtrip, nested Unix-socket binds and cross-ABI exec.
Optional foreign-owner (root) and external i386 musl probes were skipped by
the suite. No VM was started.

### Steam pressure-vessel peer startup (2026-10-28, round 36)

In day-r43, pressure-vessel PID 4434 forks helper 4498 with argv
`i386-linux-gnu-capsule-capture-libs --print-ld.so`. At 13:33:28.319
it validates the ELF32 and `/lib/ld-linux.so.2`, then emits
`exec-peer-enter 3221225534 36`; no peer state-import/handler trace follows.
The parent immediately reports SIGSYS. This is before payload syscall
handling, not an unimplemented helper syscall. The freestanding ELF64
resident installed a peer-ABI TRAP branch, whereas the bionic resident
already allowed peer startup. Kernel exec resets SIGSYS handlers, so an
Android-linked i386 peer cannot run its linker through that branch.

Allow peer startup in both resident builds. The new peer installs its own
routing before payload entry; Android's app filter remains in force. This
uses the same rule as the existing bionic resident and app-filter peer
branch, without helper/application exceptions. The early-linker regression
in `cross-exec.sh` kernel-execs an ELF32 sibling which deliberately calls
mmap2 outside the raw gateway before importing state, then enters the real
resident and completes both ABI handoffs with VFS, descriptors and SysV
state intact. Against the old filter, host strace identifies
`AUDIT_ARCH_I386 / __NR_mmap2 (192)` followed by SIGSYS; the fixed filter
passes. The device capture does not identify its first linker syscall.

The destination disconnect is a separate failure: the upstream portal
4108 is reaped with status 11 at 13:33:00.669; its replacement 4472 receives
the Spawn body and seven SCM_RIGHTS fds at 13:33:29.859 and is reaped with
status 11 at .958. Launch-client 4434 exits 125 after reporting connection
closed. There is no fatal PC report in this capture, so its SIGSEGV cause
is not established. Earlier nested bwrap 4433 separately reports missing
`/proc/self/fd/34` at 13:32:45.356. CEF itself has not been shown to start.

The worker also had a concrete diagnostic bug: its fd sweep closes original
stderr, then its portal setup skipped restoring fd 2. Restore it from the
saved diagnostic sink before applying caller fd mappings. A host-command
regression starts a portal-shaped child with no stdio mappings and checks
that fd 2 exists and the stderr write succeeds. This restores diagnostics;
it is not a claim to fix the portal SIGSEGV.

Coordinator verification: freshly launch traced Steam, confirm the ELF32
`--print-ld.so` helper imports state and the SIGSYS warning disappears.
Retain portal stderr and capture the Android crash buffer/tombstone for any
portal SIGSEGV (include executable, PC and maps); also retain the nested
bwrap argument bundle around any missing fd source. No VM was started.

Validation: the full repository host suite passed with SDK 36; compat-proxy
suite includes the new peer-startup and portal-stderr regressions, stock
Flatpak/bwrap resident-worker launch, SIGCHLD routing and Wayland roundtrip.
Android x86_64 resident/static and i386 loader builds and device-tree
preflight passed. Optional external i386 musl/foreign-owner probes retain
the suite's documented skips. Device confirmation and the portal crash fix
remain outstanding.

### Steam return-ABI startup and portal snapshots (2026-10-28, round 37)

The day-r44 warning is current, not just accumulated webhelper output:
helper 6642 enters its ELF32 peer at 14:05:07.153, never imports state, and
pressure-vessel 6533 reports signal 31. Earlier i386-native filters remain
installed after the intermediate ELF64 exec. Allowing only the peer branch
of the latest filter cannot exempt the older filter's native branch.

Ship a freestanding i386 peer (`libmatonos-v3-loader-i386-static.so`) and
select it for cross-ABI exec, symmetrically with the existing ELF64 static
peer. Both start exclusively through the fixed raw gateway until installing
SIGSYS. Keep the bionic i386 loader available for explicit initial resident
launches. Cross-ABI peers use the freestanding path, without resident bionic
GPU thread adoption, as the ELF64 peer already does; the existing forwarding
interfaces remain available. Device GL32 verification remains necessary.
The negative round-trip fixture installs an early mmap2 linker stub on the
return leg and verifies SIGSYS; the real static peers complete the round trip.
Packaging tests execute the actual extracted NDK static peers through both
ABIs and reject an interpreter in either static peer.

Portal 6335 exits with status 11 at 14:05:24.203. Immediately beforehand its
thread 6649 reports adoption misses with zero live records. Host-command's
raw snapshot/server/worker/command forks bypassed normal clone-return
adoption. Rebase each child while inherited FS still identifies its caller,
before hc_exec clears TLS. This changes only child-local registry/altstack
bookkeeping; it does not retire payload siblings or change signal routing.
The guard starts with multiple inherited records, clears TLS after the raw
monitor fork and checks that the caller alone remains discoverable. Removing
the hook makes it fail (exit 62); with the hook it passes. The resident
HostCommand integration also runs the stock nested bwrap display probe,
SCM_RIGHTS, environment, exit-status, signal and teardown cases, including a
pre-exec payload sibling. The original singleton fixture self-healed without
the hook and was insufficient as a regression.

The capture contains no portal fatal PC/tombstone, so this fixes a proven
bookkeeping defect, not a symbolized explanation of that SIGSEGV. Another
failure remains: nested bwrap 6654 resolves fd 34 to a backing `.v3-inodes`
path outside its current logical home, then exits 1 (14:05:08.954). This can
still prevent CEF from starting after the two fixes; do not interpret absence
of a window as proof either fix failed.

Validation: repository host suite (MATON_AOSP_ROOT=/mnt/data/aosp), full
compat-proxy suite, extracted NDK peer execution/packaging, NDK x86_64
resident/static and i386 resident/static builds, SDK-36 Java compilation,
and device-tree preflight passed. No VM, kernel, shared-device-tree or AOSP
changes. Coordinator: freshly launch traced Steam; verify the ELF32 helper
imports state, follow the host-command adoption lines for each portal fork,
and check for portal status 11. Retain portal crash PC/maps if it recurs and
the fd-34/bwrap argument bundle if that independent source-path failure remains.

### Steam round 38: distinguish fork inheritance from peer GPU loss

The round-45 capture's missing-bootstrap warnings come from PID 8587 after
an ELF64 -> ELF32 peer kernel exec (14:33:06.895 / 14:33:06.943), with a
freestanding i386 handler stack. They do not demonstrate that the raw
host-command fork rebase removed a bootstrap pointer. Kernel exec removes
the old mappings; the new i386 static peer has no MATON_GPU_BRIDGE startup
and never creates the named GPU bootstrap. Keeping old pointer strings
across that exec would produce dangling addresses, not working adoption.
The r37 SIGSYS startup fix remains on this branch, but its loss of resident
GPU support is **unresolved**. Do not merge the r37 series as a complete
Steam fix based on these host tests.

A separate proven overhead is fixed: Vulkan bootstrap discovery reads maps
in blocks instead of one byte per trapped syscall. The day-r45 capture ends
while PID 8587 is still doing one-byte reads; missing-bootstrap retries scan
maps repeatedly. No negative discovery cache is introduced, because a
resident can publish the bootstrap later. The regression checks chunk
boundaries, bounded backend reads and discovery following a previous miss;
its old-runtime negative control fails. A second regression forks before
loading the actual Maton Vulkan ICD in the child and creates/destroys a real
lavapipe instance using the inherited named bootstrap. This validates fork
mapping inheritance and ICD discovery with a host bridge/adoption fixture;
it does not validate the Android resident adoption backend or cross-ABI GPU
reinitialisation.

Validation: full compat-proxy suite, Vulkan-forward suite, NDK loader builds,
NDK compilation of changed runtime.c for both ABIs, and device-tree preflight
pass. Vulkan's benchmark harness needed its existing altstack assembly added
to the launcher link. A separate full NDK Vulkan build hits the existing
ELF64_ST_VISIBILITY dependency in wayland_resolve.c (missing from NDK elf.h).
The repository suite stops at OSTree's 3% free-space threshold on /mnt/data;
no shared fixtures or other jobs' files were deleted. No VM or push.

Coordinator follow-up: the buffered scanner commit can be cherry-picked
independently of the rejected r37 series. A GPU-capable cross-ABI resident
startup needs a design that starts bionic under inherited native-ABI TRAP
filters; simply selecting the bionic peer again restores the earlier SIGSYS.
The present work does not meet that requirement and does not claim to fix
the webhelper restart loop. A traced fresh Steam launch can establish how
much of the steamsysinfo delay was the maps scan after this isolated change.

### Steam round 39: static-peer GPU startup remains unresolved

Rebased on main (`fbdd2bd`; already an ancestor). No production change was
made in this round, and this branch is still not ready to merge as a Steam
fix. The static i386 peer's symbol table has no `gpu_bootstrap`,
`adopt_install` or `adopt_api_address`; the bionic peer has all three and
an undefined `dlopen@LIBC` resolved by Android's dynamic linker. This is a
missing native runtime, not merely a missing environment variable.

The local AOSP source `bionic/libdl/libdl_static.cpp` also rules out simply
linking the static peer against libdl: its `dlopen` and `dlsym` return null
and its error identifies libdl.a as a stub. A correct implementation needs
to start a fresh bionic runtime after the freestanding gateway has made
inherited TRAP filters safe, then install real adoption/bridge callbacks.
It must keep the gateway used by those callbacks exempt from every older
filter, including filters from the original ABI on a return exec. Merely
mapping the dynamic peer at an arbitrary address would relocate its raw
gateway and risk recursive SIGSYS when its handler calls RAW. Passing old
bootstrap addresses over kernel exec would instead reference unmapped memory.

Re-ran the fork-to-fresh-ICD lavapipe test: it passes. That host fixture uses
a host bridge/adoption provider and preserves mappings across fork; it is
not evidence that a static peer can recreate Android GPU state after exec.
The required x86_64-to-static-i386-to-ICD `vkCreateInstance` acceptance test
and working bootstrap implementation remain undone. No VM was started.
