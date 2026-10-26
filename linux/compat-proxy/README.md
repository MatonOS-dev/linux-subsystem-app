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
can replace.

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

The resident ELF loader maps static, glibc and bionic ELF images, their
interpreters, argv/env and auxv. Resident exec keeps the proxy in the process,
resets ABI-specific TLS and closes payload CLOEXEC descriptors. Cross-class
exec validates the trusted sibling executable and transfers pointer-free
runtime state through a sealed memfd. Signals are forwarded/chained with the
payload ABI context; clone/fork, thread exit, robust lists, and selected
pthread TLS behavior are adapted by architecture backends.

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
