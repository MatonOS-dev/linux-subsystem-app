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
