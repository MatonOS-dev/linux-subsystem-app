> v3.5: linuxd is retired. Any linuxd/shared-store paths below describe the
> historical design. See [current ownership and device checks](../../docs/RETIRE-LINUXD.md).

# Linux layer v3 — syscall compatibility proxy

The x86_64 resident ELF loader and static library. Android's inherited
seccomp filter, UID and SELinux restrictions stay in force. No real namespace,
mount, ptrace, or Android change. The virtual filesystem is compatibility
state, **not containment**. Production stubs launch the Android-linked loader
from `V3Launcher`; its optional `--gpu-bridge` mode installs x86_64 thread
adoption and the read-only GPU bootstrap before entering the Linux payload.
The i386 loader remains freestanding and does not accept this option. Host
test binaries remain static and do not include the Android bridge backend.

Build/test: `./build.sh`, `./tests/run.sh` (host cc, -Wall -Wextra -Werror).
Outputs: `build/linux-layer-v3`, `build/liblinux-layer-v3.a`.

```
build/linux-layer-v3 --android-filter /bin/bash -c 'printf hello | cat'
build/linux-layer-v3 --android-filter /usr/bin/bwrap --ro-bind / / --tmpfs /tmp --proc /proc --dev /dev /bin/true
build/linux-layer-v3 --android-filter --root /runtime/root /bin/bash
```

`--android-filter` copies this checkout's generated x86_64 app allowlist.
Omit it under an already filtered Android app. The harness installs this
filter after initial loader setup; it does not reproduce app UID, SELinux,
Android filesystem permissions, or prove embedding into an already filtered
process. Input hashes are in `build/policy-inputs.sha256`. The full syscall
complement, with translation/virtual/ENOSYS classifications, is generated in
`build/trapped-syscalls.{json,md}` on each build.

`build-android.sh` produces the production x86_64 loader as an Android-linked
executable so bionic remains resident while adopted Linux threads call the
Vulkan bridge. The normal launch path still enters through `proxy_enter`; only
`--gpu-bridge` selects adoption. That option also publishes the bridge and
adoption pointers in the named read-only `maton-gpu-bootstrap` mapping used by
the freestanding Vulkan ICD.

Legacy path calls translate to the allowed *at syscalls before replay. Other
translations include dup2 (including same-fd validation), getpgrp, pipe,
poll/select/pause, accept, fork, getdents layout conversion, epoll, inotify,
eventfd/signalfd, timestamps, alarm and time. Unsupported calls return ENOSYS;
we never replay a forbidden syscall. io_uring remains deliberately disabled.

The virtual filesystem dynamically allocates mount entries and paths. It
resolves cwd/dirfd-relative paths, dot components and up to the kernel's 40
symlink traversals; absolute payload symlink targets resolve in the virtual
root. Cwd is recorded after successful chdir, inherited by fork; fchdir uses
/proc/self/fd reverse mapping. getcwd and proc fd readlink expose virtual
paths; proc self exe readlink exposes the loaded executable. rename/link
between different virtual mounts return EXDEV, even when backing files are
on the same physical filesystem. A sixteen-entry mount-lookup cache never
caches symlink contents or existence and is invalidated on table changes.

Bind/rbind, remount metadata, tmpfs, proc, devpts/mqueue, pivot_root, chroot and
umount2 operate on the table. Recursive bind copies existing virtual
submounts. Tmpfs creates a private 0700 backing directory under TMPDIR (set
this to the app cache/tmp on Android; host default /tmp); proc/devpts/mqueue
map to the real corresponding directories. A mountinfo snapshot enables
bwrap's post-bind verification. Namespace requests are recorded and removed
from clone or accepted as unshare no-ops. uid/gid map and setgroups writes
use anonymous memfds; credential setters/capability operations are virtual
setup acknowledgments, granting no kernel privileges. Existing sync pipes,
eventfds, and bwrap/Flatpak seccomp installs remain real; stacked filters
also apply to the handler's raw syscalls.

CLI `--root` accepts the coordinator's symlink scaffold: its existing
usr/bin/lib/lib64/etc/sbin/proc/dev/data/system/apex endpoints are resolved through the
host kernel and pinned as explicit mounts before payload symlink semantics
begin. This distinguishes bootstrap scaffold links from payload links.

Exec supports static and glibc ELF and shebangs. CLOEXEC descriptors close on
successful in-process exec. ELF program-header and argument/environment
vectors grow dynamically, with overflow checks. A 4 MiB minimum initial
stack grows for larger argv/env; PT_INTERP follows Linux's 4096-byte pathname
limit and script parsing uses Linux's 256-byte header size. Thread records
are dynamic; the previous 128-slot cap is gone.

The proxy's per-ABI seccomp filters pass only exact `fcntl(F_GETFD)` and
`fcntl(F_SETFD)` commands directly to the kernel. Their state is kernel-owned,
and this avoids trapping libc's close-on-exec scan of every descriptor up to
`RLIMIT_NOFILE`; duplication, status flags, locks and all other `fcntl`
commands still update proxy state in the trap handler. Resident exec reads
the kernel flag when it closes tracked payload descriptors. It preserves
proxy-private descriptors across resident exec even if payload code sets
`FD_CLOEXEC` on them; `F_GETFD` may therefore expose a private descriptor's
real kernel flag, while the proxy continues to reject payload I/O and explicit
close on that descriptor. For mixed-ABI kernel handoff, the sealed state fd is
made inheritable immediately before `execveat(AT_EMPTY_PATH)` and its prior
flag is restored if exec fails.

Bionic ELF uses the resident loader too: `/system/bin/linker64` is mapped as
PT_INTERP, with PHDR/PHNUM/ENTRY/BASE/RANDOM/SECURE/PAGESZ/EXECFN plus inherited
HWCAP/HWCAP2/SYSINFO_EHDR auxv. In-process exec resets FS TLS before entering
the next linker. Android's debuggerd SIGSYS registration goes through the
same handler-chaining guard as glibc registrations. No native kernel exec
is used. `/proc/self/exe` stat follows the loaded image, as does readlink.

On Android, start bionic tools without `--root`: the initial payload mount
table is empty and `/system`, `/apex`, `/data` retain their Android meanings.
Only bwrap's setup/pivot supplies the Linux payload root. The table, root,
cwd and fd names are inherited in process memory through fork (copy on
write), retained across resident exec, and private to the child after
namespace changes. Threads share the table; clone requesting both a new
namespace and CLONE_VM is rejected with ENOTSUP rather than sharing namespace
mutations. Thread-local unshare namespaces and CLONE_FS semantics remain a
spike limitation. Initial host fixtures use `--root` to supply Android's
filesystem; `--map SOURCE TARGET` overlays Android paths on the real host root
for the glibc bwrap payload tests.

`tests/musl-host.sh` copies the staged static musl subsystem tools
inputs into build/
and verifies bwrap 0.13.0 and Flatpak 1.19.2 versions under --android-filter,
then runs staged bwrap with the two host glibc payloads. Set COMPAT_PRODUCT_OUT
to use different artifacts. fd names are recorded across opens, close, dup
and fcntl duplication so fd mount targets preserve their opening aliases.
Pivot preserves the old root's submounts. openat2 returns ENOSYS: current
bwrap uses its checked openat fallback, avoiding incorrect RESOLVE_IN_ROOT
semantics. Traps carry pid=; every mount has an adjacent mount-owner record.

SIGSYS remains deliverable across rt_sigaction/sigprocmask, sigreturn and
app altstack changes. The proxy preserves changes to other signal-mask bits
across sigreturn (required by bwrap's SIGCHLD signalfd monitor). Its private
1 MiB altstack is retained; chained app SA_RESETHAND and actual
SYS_USER_DISPATCH delivery are tested. App SA_ONSTACK semantics and multiple
application-requested user-dispatch is handled by the proxy; resident thread
adoption itself does not enable syscall user dispatch. Thread records/private stacks are
not reclaimed after thread exit.

Remaining constraints: user pointers are not EFAULT-safe; mount/cache/cwd
mutations are not synchronized for concurrent pathname calls; reverse mapping for descriptors not opened/duplicated through the proxy
is ambiguous when multiple aliases share one backing path; mountinfo
is a minimal setup snapshot (duplicate IDs, rw flags, no escaping), not a
full proc implementation; tmpfs backing directories are not cleaned up;
virtual read-only checks cover intercepted pathname mutations, not writes
through already-open descriptors or shared mappings; cwd outside a changed
root and concurrent CLONE_VM without CLONE_FS need more work. Exec lacks
atomic rollback, old-map/TLS reclamation, complete signal reset and
multi-thread teardown. Temporary signal-mask translations cannot supply all
kernel atomic-wait semantics. These are compatibility gaps, not privileges.

Coordinator VM runner: `tests/vm-stage.sh` pushes only to
/data/local/tmp/compat-proxy and pulls stdout/stderr/status. Set
ANDROID_SERIAL and COMPAT_RUNTIME_ROOT; the root should expose /data,
/proc and /dev. Optional COMPAT_BWRAP must name an existing **glibc** bwrap.
Set COMPAT_STAGE2=1 and COMPAT_RUNTIME_UID to the installed
org.matonos.linuxruntimes UID to run Stage 2 as root: the static musl Flatpak CLI
uses user-0 app UID 10115's DE install and the runtime app's DE install,
linuxd's global CLI HOME/XDG directories, and CE home for the bwrap shim.
TMPDIR/download tmp are deliberately confined to the test staging directory.
The static musl CLI gets no --root. stdout, pid-tagged trap/mount logs and status
are pulled even when Flatpak fails. This executes existing deployments; it
does not represent production launch identity/portal integration.
It requires no Soong/Gradle/image build and performs no VM reboot or policy
change. VM tests were not run in this sandbox. Result report:
`/mnt/data/aosp/out/pc-logs/agents/compat-proxy-3-result.md`.

Round 4: a virtual NEWNET's NETLINK_ROUTE socket is an eventfd-backed
readiness carrier with a per-process ACK queue. bwrap's IPv4 127.0.0.1/8
RTM_NEWADDR and IFF_UP RTM_NEWLINK for virtual lo (index 1) receive
NLMSG_ERROR success ACKs with the request sequence, port ID and embedded
request header. Unsupported requests receive EOPNOTSUPP ACKs; malformed
messages return EINVAL. No route send reaches the host kernel. sendto/recvfrom
and scatter/gather sendmsg/recvmsg, sender address, MSG_PEEK/MSG_TRUNC,
poll readiness and descriptor duplication/close are covered. The AF_INET
SOCK_DGRAM control socket used for SIOCGIFINDEX("lo") is virtual too, so
setup does not need a real IP socket. Other descriptors retain raw behavior.
Namespace flags on clone now become active only in the child, not its parent
monitor. Virtual userns sysctl writes use memfds and a child-namespace budget,
allowing bwrap's --disable-userns setup/check without changing the host's
user.max_user_namespaces. This implements that setup sequence, not general
Linux user-namespace accounting or a security boundary.

`tests/musl-host.sh` now also runs `tests/flatpak-bwrap-host.py`. Its
Flatpak 1.19-shaped fd bundles use --ro-bind-fd for /usr and /app,
--ro-bind-data for generated config, --bind-fd for the cache, user/PID/IPC/net
namespace setup, --disable-userns, proc/dev mounts, usr links and sysfs binds.
Sealed --args data drives the staged static musl Flatpak multicall and bwrap. The ldconfig -X
-C phase must produce a cache. The bash phase verifies --file plus
--ro-bind-data /.flatpak-info, --info-fd, --json-status-fd setup/exit events,
--block-fd release and --sync-fd EOF. Both phases install the nondevel,
nonmultiarch x86_64 rules from Flatpak 1.19.2 setup_seccomp, exported by the
host libseccomp; the payload filter adds getppid -> EACCES for an observable
filter check. Set COMPAT_HOST_RUNTIME to a runtime's files/ directory to
exercise its ldconfig/bash; default /usr uses the host glibc tools. Expanded
argv and the actual exec argv are saved in build/flatpak-bwrap/*.argv.json.
This is a source-derived fixture, not a replay of the VM's exact deployments,
extensions, environment or Flatpak-generated filter bytes. Stage 2 now adds
Flatpak -v so the next VM log includes expanded --args contents.

Network spike limits: no general virtual IP stack or route enumeration;
empty ACK queues return EAGAIN even for a blocking receive. Kernel carrier
flags/type/stat differ from an actual netlink socket. ACK state is private
copy-on-write memory; concurrently using the same endpoint after fork or
SCM_RIGHTS transfer is unsupported, as is concurrent thread mutation.
Namespace sysctl reads and kernel namespace IDs still show host values.
Filters remain installed in the kernel and constrain raw operations; virtual
answers are not a full interpreter for additional seccomp policies (TRAP has
priority over ERRNO). Do not infer containment from these host tests.

Bwrap loads now log outer argv and NUL-separated `--args` bundles through
pread without consuming or changing the fd. `tests/flatpak-bwrap-host.py`
accepts `COMPAT_STAGE2_LOG` to replay the logged outer ldconfig command.
The fd bundle remains source-derived host test data: the round-4 log has no
expanded bundle and no app-launch invocation, so this does not reproduce
all VM deployments, metadata, extensions or permissions. Instance files,
locks, status/lifetime fds and actual seccomp installation are exercised by
the offline payload phase. No storage layout is hard-coded into the proxy.

Round-6 offline app-launch replay (after `tests/musl-host.sh` prepares the
Android linker/APEX fixture):

```sh
COMPAT_STAGE2_LOG=/mnt/data/aosp/out/pc-logs/agents/v3-round5-vm-logs/stage2.log \
COMPAT_HOST_RUNTIME=/home/hanro50/.local/share/flatpak/runtime/org.freedesktop.Platform/x86_64/26.08/active/files \
  python3 linux/compat-proxy/tests/logged-bwrap-host.py
```

This replays the full logged app bundle in order, with host-backed source
paths/fds and inert extension fixtures; the logged destinations, environment
and bash command are retained. It adds status/block/sync fds and `--cap-drop
ALL`, checks successful payload/monitor completion and hashes runtime `/etc`
before/after. `build/logged-bwrap/argv.json` records both the original bundle
and adapted host arguments; `out`, `log`, `info.json` and `status.json` hold
execution evidence. This does not execute a real installed Flatpak application
or establish Android app-domain containment.


Round 7 packaging: `bash build-android.sh` builds the freestanding x86_64
`libmatonos-v3-loader.so` with NDK r30/API 36 into
`${MATON_V3_ANDROID_OUT:-/mnt/data/aosp/out/matonos/compat-proxy}`. Gradle packages
it in the compositor host APK. The stub passes no --android-filter; its filter
is inherited. `--parent-pid <pid>` sets SIGKILL parent-death handling and rejects
a stale parent. Host lifecycle validation:

```sh
python3 tests/lifetime.py /mnt/data/aosp/out/matonos/compat-proxy/libmatonos-v3-loader.so
```

See ../subsystem/V3-LAUNCH.md for the production spec, paths, retirement and
remaining image checks. The musl host fixture stages tools at the subsystem
APK native-library path used in production.

DNS uses a per-process helper thread and a UDP socket bound to 127.0.0.1 on an
ephemeral port. On first DNS use, the proxy creates that thread with a raw
`clone` syscall on its own mmap'd stack. The exact post-syscall gateway PC is
allowed through both seccomp filters so the kernel can preserve the supplied
child stack instead of entering the app-stack `clone_retry` trampoline. The
assembly wrapper restores the parent's callee-saved registers; the child uses
no libc or TLS. For each query it opens
`/dev/socket/dnsproxyd`, sends netd's NUL-terminated `resnsend 0 0 <base64>`
command, then polls that fd alongside its UDP socket. Replies use netd's
big-endian status and length header followed by the DNS packet. Responses are
returned to the original client address. The proxy records source ports of its
own DNS sockets and the helper drops datagrams from every unrecognized port. UDP connect/sendto/
sendmsg/sendmmsg keep the app's original socket and rewrite only the configured
loopback endpoint; peer and receive source addresses are reported as the
configured endpoint. DNS TCP connect returns `ECONNREFUSED`. Ordinary Internet
sockets retain normal readiness and address behavior. Each fork child starts
its own helper lazily. `tests/dns-translation.c` covers endpoint parsing,
TCP refusal, ordinary socket names, and endpoint matching with mocked syscalls.
`tests/dns-helper.c` exercises the asynchronous loop against a mock AF_UNIX
dnsproxyd and real host UDP/epoll/pthreads. It checks the command bytes against
a fixed libnetd_client vector, error and truncated replies, connected UDP, sendto/sendmsg/
sendmmsg, parallel queries, poll/epoll, blocking reads, `MSG_PEEK`, foreign
source rejection, fork restart, and no wakeups on an ordinary socket. Device
testing against netd is still required. `tests/raw-clone-gateway.c` verifies
custom-stack thread creation through an exact-PC seccomp exception.

O_TMPFILE deliberately returns EOPNOTSUPP after normal virtual mount policy
checks. Callers use named temporary files and rename; no anonymous-inode
tracking, descriptor ownership, fork/exec hooks or proc-fd publication exists.
Proc-fd link sources receive the same non-store refusal as other external paths.
`tests/links.c` covers this fallback and `tests/publication.c` covers named
publication and rename mount policy.

Hardlink emulation (links-simplify, 2026-10-17): only the nearest owner-owned
regular `.v3-store-root` establishes a store; both link names must share it.
Unmarked link/linkat calls use the kernel answer without creating a store.
An indexed placeholder cannot be linked or moved outside its store (EXDEV),
including on hosts whose kernel permits native hardlinks.
The marker remains visible and payload pathname mutation is denied. The owner
creates markers before starting the proxy. Existing symlink-format stores
are not migrated: discard old disposable staging trees before using this build.
Do not mix proxy versions on a live store.

On first link, content moves into `.v3-inodes/i-<128-bit-random-id>`.
Each linked name is an empty regular file with S_ISVTX and an index entry
`p-<16-hex-st_dev>-<16-hex-st_ino>` containing the NUL-terminated relative
backing ID. Both the sticky bit and a valid index are required. A stray sticky
file without an index is ordinary and its metadata/content remain unchanged.
Relative index values need no root-move rewriting. Payload chmod preserves the
physical sticky bit on ordinary files and cannot set it on backing objects;
chmod of a linked name affects only its backing object, preserving its marker.

Open (including O_PATH/O_NOFOLLOW), pathname stat/xattrs/truncate/chmod/chown/
utimens and related metadata operations redirect to the backing regular file.
The fd then supplies read/write/mmap and identity naturally; logical nlink is
read from the persistent count sidecar. Reopening O_PATH through proc-fd opens
the backing file. There is no symlink type masking, dirent type rewriting,
readlink override or no-follow flag rewriting. Native readlink(path) returns
EINVAL; native readlinkat(regular_fd, "") returns ENOENT on the host kernel.
openat2 retains its existing ENOSYS result; libglnx's checked fallback works.
`.v3-inodes` remains hidden and payload paths cannot access its contents.

The existing flock, conversion journal and incremental intent protocol protect
namespace/count updates from process interruption. A placeholder is indexed
under a single hidden staging name before atomic publication. Recovery deletes
unpublished staging files and their indices. Unlink/overwrite retire the old
placeholder index; rename/exchange retain identities still at either affected
name. Zero logical links collect backing content and count metadata. Existing
fds keep working after final unlink. Clean operations never scan the tree.
Corrupt/missing count metadata permits a bounded exceptional recount.

Limits: no fsync ordering or power-loss guarantee; direct host/raw namespace
mutations bypass the protocol, including inode reuse against a stale index.
Corrupt index/intent recovery is not a general filesystem repair service.
Pre-existing native hardlink aliases are not imported. Recounts require readable
same-filesystem directories. Android SELinux integration still needs a fresh
image test; no VM was touched for this change.

Packed path descriptors (exec-packed, 2026-10-18): indexed `O_PATH` opens use
a real kernel `O_PATH` descriptor and retain the indexed object alongside it.
This matters to libglnx's checked `openat2` fallback: `statx` can see a packed
entry, but the following `openat(O_PATH|O_NOFOLLOW)` must also succeed. Packed
directories and files preserve their indexed type in `fstat`/empty-path
`statx`; an indexed symlink opened with `O_PATH|O_NOFOLLOW` is represented as
a symlink fd, and `readlinkat(fd, "")` returns its indexed target. Such a
descriptor remains usable as a dirfd through the existing virtual fd-name
tracking. Physical Flatpak deployment `files/<relative-path>` probes map back
to the mounted pack index, since Flatpak checks these paths before bwrap
applies its binds. Packed statx advertises a synthetic mount ID unique to
each live pack mount, which libglnx requires alongside type and inode when
validating a chased `O_PATH` fd. The pivot/exec fixture includes Flatpak's
bwrap usr-merge links
(`bin -> usr/bin`, `lib64 -> usr/lib64`) and exercises the packed directory
and symlink chase through both the physical deployment path and pivot root
before loading the packed ELF interpreter.

Tests cover marker scoping, concurrent independent proxies, shared writes and
metadata, same-inode rename, root relocation with relative indices, final-unlink
GC, O_PATH/proc-fd reopen, real symlinks and raw regular-file/statx checks outside
the proxy. Six constructed conversion checkpoints and a real SIGKILL intent
exercise recovery. The 20k-file/5k-link benchmark requires zero healthy-path tree
walks. Optional host OSTree init/commit/require-hardlinks checkout/fsck runs when
ostree is installed. Set MATON_TEST_GLNX_SOURCE to compile the real libglnx
chase/reopen regression. Run `bash linux/compat-proxy/tests/run.sh` with its
build symlink pointing to external output, then `tests/run.sh OUTPUT` and the
signed APK build described in the repository README.

Round 15 GPU sysfs: at startup the freestanding loader reads the five
`vendor.maton.graphics.{vendor,device,driver,render_node,mode}` properties
from bionic's `vendor_maton_graphics_prop` area (readable by every domain). It validates the area version, trie
bounds, property names and short-value serials. Missing, dirty, malformed,
software-mode or unusable render-node snapshots leave paths unchanged. No
PCI sysfs is read. The published render node's actual character-device
major/minor comes from opening and fstat'ing `/dev/dri`; a primary node is
included only when that directory contains exactly one primary and one
render node. Multi-GPU card association requires the detector to publish
more identity; render and card indices are not assumed to match.

A private 0700 tree below the inherited TMPDIR supplies read-only virtual
mounts at `/sys/{dev,devices,bus,class}` using the existing tmpfs/bind-backed
VFS approach. This reuses the resolver and real backing directory/file
operations rather than adding separate syscall-specific fabricated stat and
getdents results. It is a virtual mount only; no real mount is performed.
The generated PCI directory supplies vendor/device, zero subsystem IDs,
revision `0x00`, class `0x030000`, driver/PCI-ID/slot uevent, subsystem link
to `/sys/bus/pci`, and DRM children with device/subsystem links and
major/minor/devname uevents. PCI slot `0000:00:00.0`, revision and subsystem
IDs are synthetic because the detector publishes no corresponding fields.
They must not be used as physical topology or board identity.

Source sequence checked against `external/libdrm/xf86drm.c`:
`drmParseSubsystemType` reads the subsystem link; `get_pci_path` uses
realpath; `drmParsePciBusInfo` reads PCI_SLOT_NAME from uevent;
`parse_separate_sysfs_files` reads the five numeric identity files;
`drmGetDevice2` lists the DRM children. Mesa's `src/loader/loader.c` uses
`drmGetDevice2` and reads the same vendor/device paths. The synthetic device
is reported as PCI, including a virtio GPU's exported PCI identity.

`/sys/bus` exposes only an empty `pci` directory, and `/sys/class` only an
empty `drm` directory: this is not general udev/device enumeration. Their
exact absolute paths use a fast resolver answer only while the current
mount source still matches the generated tree, including after a recursive
bind/pivot. Other sysfs branches keep normal behavior. On this host, 40,000
opens under the copied app filter took 4.758 seconds before the fast path
and 2.354–2.358 seconds afterward. This measures the generated-tree fixture,
not ETR or Android SELinux performance. Existing namespace/thread mutation
limits apply. Generated directories share the existing tmpfs backing
cleanup limitation and survive until the app cache is cleaned.

Run `bash tests/gpu-host.sh` (also included by `tests/run.sh`). Its startup
fixture constructs the actual bionic property-area ABI and substitutes only
raw fstat identities for two ordinary fixture files; all remaining syscalls
reach the host kernel. Payload checks run under the copied Android app
filter: both render/card paths, realpath, lstat/readlink, relative openat,
PCI reads, statx, subsystem target inode, DRM getdents and write rejection.
Software, missing/partial properties, malformed header/trie and dirty serial
cases retain normal paths. Missing render-node and ambiguous-card cases are
covered; the generated tree also survives host bwrap recursive bind/pivot. Host suite and NDK r30/API-36 loader build passed.
No adb, image build or hardware test was performed.

Policy: `vendor.maton.graphics.*` is labelled `vendor_maton_graphics_prop`,
which every domain may read; apps get no read of the wider `vendor_maton_prop`.

Fresh-image hardware validation for the coordinator/user:
1. Apply the property read grant, package this NDK loader and boot a fresh
   image; keep the VM idle-suspend timeout disabled for VM tests.
2. On the Ryzen/RX6600 PC, Surface Pro 3 and Haswell ProDesk, launch an
   ordinary Flatpak stub and check the detector's vendor/device/driver/node
   properties against the GPU reported by glxinfo and vulkaninfo/vkcube
   (where Vulkan is supported). Confirm iris/crocus/radeonsi/RADV selection
   and absence of the Mesa device-information warning.
3. Repeat with software rendering or an absent render node: boot and app
   startup must still succeed through the existing software path.
4. On a multi-GPU PC, verify the selected render identity and that unrelated
   primary cards are not falsely advertised. Full card topology is deferred
   until the detector exports an unambiguous card/slot identity. The detector
   currently reads IDs from its primary PCI adapter but may pick a different
   first supported render node; multi-GPU snapshots need that association
   corrected in the detector before relying on them.
5. Run ETR and compare `/sys/bus`/`/sys/class` polling time/count with prior
   logs; check controller behavior separately because this subtree does not
   enumerate controller hardware. No kernel or AOSP changes are required.

### Round 17: tmpfs cache garbage collection

Loader startup collects stale `compat-proxy-<pid>-*` directories in TMPDIR.
New backing directories carry `.v3-owner` and an exclusive directory flock
held by a reserved proxy descriptor. Forked children inherit the same lock,
and resident exec retains it, so a parent's exit cannot collect bind-data
still used by its descendants. Payload close sweeps cannot release these
reserved descriptors; dup/fcntl attempts involving them fail with EBADF.
Collection uses an independent open file description and a nonblocking flock,
which identifies lifetime without relying on a reusable numeric PID. Creation
and collection serialize briefly on the cache directory flock. Legacy dirs
without a marker are retained whenever the named PID exists or proc lookup
fails for reasons other than ENOENT. Recursive removal uses directory FDs
and O_NOFOLLOW, and never follows symlinks out of the cache subtree.

`tests/run.sh` now includes temp-gc.c: active inherited lock, dead owner,
unlocked marked directory whose numeric PID is live, legacy live PID and
symlink containment. The full host suite and the freestanding NDK/API 36
loader build passed. This collector targets tmpfs backing dirs; the separate
`v3-gpu-*` synthetic sysfs cache has not been added to its scope.

Fresh-image check: record the app's cache directory names, launch/quit/relaunch
several Flatpak apps, and verify stale compat-proxy dirs disappear on the next
loader start while the current launch's bind-data files remain readable.
Repeat with a child process still alive after its parent exits. No cache
or installed app data should be removed outside these backing directories.
Round 18 crash reporting: resident exec now resets caught signal actions (all
except the resident SIGSYS action), preserving SIG_IGN as kernel exec does.
This prevents bionic debuggerd handlers from using glibc TLS after a Linux
payload starts. Default fatal actions report signal/code, real pid/tid, fault
address, PC and the raw maps entry to stderr (forwarded by the stub to
MatonV3), then re-raise the original signal. Resident ELF segments also report
their source path and ELF virtual offset because they are anonymous mappings.
App-installed fatal handlers may replace this default reporter. Trace mode
logs AF_UNIX addresses, including abstract names, before path translation.

Host regression: compile `tests/exec-signals.c` with host cc, run it through
`build/linux-layer-v3 --android-filter`: it checks caught/ignored actions over
two execs and deliberately segfaults; expect signal 11 and `v3 fatal image=`.
The existing full host suite and the NDK loader build passed. Rio itself has
not been reproduced here: the old device trace lacks a fault PC, so its
original platform fault remains unresolved until a new diagnostic launch.

### DRM class and socket trace checks (round 19)

The synthetic GPU tree now exposes class/drm links for exactly the render
and unambiguously associated primary nodes already published under dev/char.
It preserves real node names: card1 remains card1. It does not invent card0,
boot_vga or PCI config bytes. Mesa's device-select boot-VGA scan may therefore
return no preference; that is a normal selection fallback, not a WSI wait.
The detector does not supply the real boot-VGA identity needed to publish it.

`tests/gpu-host.sh` checks class/dev aliases resolve to identical inodes and
retains the absent-device, malformed-property and ambiguous-card cases.
`tests/unix-trace-host.py` checks padded pathnames stop at NUL and abstract
addresses preserve all addrlen bytes as hex escapes. Both pass; the full
proxy host suite and NDK r30/API 36 loader compilation pass. Round 19 made
no device changes; the coordinator must verify the next fresh image.

### Terminal diagnostics and host regression (round 21)

The loader saves its original stderr with `F_DUPFD` at fd 256 or above before
loading the payload. The existing private-fd guards protect that descriptor;
`close_range` now also preserves private descriptors (including directory
ownership locks), and its CLOEXEC mode leaves them inheritable. UNSHARE mode
returns ENOSYS because the inherited app filter prevents a real files-table
unshare. All proxy trace and fatal output uses this sink, including after a
shell replaces stdio with its pty. Trace includes ioctl request numbers,
exec paths, and exit_group status. This is diagnostic plumbing, not a new
security boundary.

Termios2 copies pass through a small nonblocking pipe so kernel user-copy
checks return EFAULT for inaccessible input/output or read-only output, rather
than dereferencing payload pointers inside SIGSYS. The existing classic ioctl
translation and BOTHER fallback remain. Android's `unpriv_tty_ioctls` already
allows TIOCSCTTY, TIOCSPGRP/TIOCGPGRP, TCGETS/TCSETS*, window sizes and TCFLSH;
there is no reason to fake controlling-terminal or job-control state.
Optional tty calls outside that list (TIOCGSID, TIOCNOTTY, TCXONC, TCSBRK,
legacy TCGETA/TCSETA*, line-discipline controls) still depend on the actual
SELinux policy; no permission-bypassing success is fabricated.

Run `bash build.sh`, `python3 tests/pty-host.py`, and
`python3 tests/exec-signals-host.py`. The normal `tests/run.sh` includes the
pty regression. The test opens `/dev/pts/ptmx`, tests classic and termios2
attributes and invalid buffers, establishes a controlling tty after setsid,
sets/gets its foreground process group, opens `/dev/tty`, replaces stdio,
exercises fd guards and close_range modes, execs `/bin/sh -c 'echo ok'`, and
reads its output from the master. Fatal diagnostics are separately checked
after stderr redirection and resident exec. These pass with the copied app
syscall filter and also with the pty test inside host bwrap's virtual /dev.
Host tests cannot reproduce enforcing device SELinux xperms or ptmxmode=000.

Rio's immediate shell exit remains unconfirmed on device. Its old trace stops
before shell exec and cannot identify SHELL, the generated passwd shell, or
login rc behavior. The proxy does not override NSS/getpwuid: glibc reads the
sandbox's nsswitch.conf and generated /etc/passwd. Verify those values inside
the same sandbox, not via the device's root shell. After a coordinator-built
fresh image, trace Rio and inspect the child's `tty-ioctl`, `v3 exec`,
`exit-status` and fatal messages. Compare a clean non-login `/bin/sh -c
'echo ok'` launch with the configured login shell; inspect /etc/profile and
home rc files only if exec succeeds. Full device steps and evidence are in
`out/pc-logs/agents/v3-term-result.md`. No AOSP or kernel changes are needed
for the fixes here; no per-app changes were made.

### Round 22: Development.HostCommand

`--host-command /absolute/private/socket` snapshots the completed app root at
its first exec with `/.flatpak-info`. Commands received with SCM_RIGHTS fork
from that resident snapshot and use the same loader, UID, filters, mounts and
app environment. No host exec or namespace reconstruction is used. The per-app
Java portal supplies Development v1, caller ownership and WATCH_BUS; resident
subreapers handle command descendants, including detached sessions, and runtime
shutdown. See [HOST-COMMAND.md](../subsystem/HOST-COMMAND.md) for protocol,
limits, source spec, integration requirement and fresh-image device steps.
Run `tests/host-command-pair.py` for bwrap/snapshot/pty/lifecycle tests under the
copied filter. The actual listener test `tests/host-command.py` requires Unix
pathname connect permission, unavailable in the agent runner. The required
broker routing change is prepared outside the source tree and listed in the
compositor SHARED-CHANGES.md; this worktree's authorized source scope excludes
linux/dbus-broker, so the live bus cannot expose the method until it is applied.
### Round 23: native SysV IPC in the resident proxy

`sysvipc.c/.h` implement the x86_64 semaphore, shared-memory and message-queue
calls in SIGSYS. They issue no native SysV IPC syscalls and need neither
CONFIG_SYSVIPC, seccomp notification, process_vm transport nor an LD_PRELOAD
shmat helper. The implementation is original Apache-2.0 C; the public Linux
UAPI definitions supply the wire structures. The ported Apache-2.0 workload
and edge fixtures come from the earlier `linux/sysvipc` implementation.

Each initial loader creates a private namespace inherited by its forked
processes and resident execs. Separate initial loader invocations do not
join it, even with the same UID. Production app/session coordination would
need an explicit namespace handoff to join independently started loaders.
This does not add IPC or Binder access between apps.

Metadata lives in a shared, dynamically growing file mapping. Stored links
are offsets; callers map the current size while holding a process-shared
futex mutex. The mutex contains the owner's TID and start-time token. Dead
owners are detected through raw proc reads and timed futex waits. A before-
image journal rolls back interrupted metadata transactions before another
caller enters. An unreadable/corrupt recovery journal poisons the namespace
and returns EIO rather than continuing with partial metadata. This recovers
process death; it is not power-loss durability. Semaphore buffers, object
slots, process records and message buffers are reused; the arena retains its
allocated pages until namespace cache collection rather than shrinking.

Segment contents are **per-segment private-cache files, not memfds**. This is
a deliberate implementation deviation from the requested backing mechanism:
a sibling forked before segment creation must be able to open the segment
after its creator dies, without an FD broker or pidfd_getfd. A memfd alone
cannot provide that lifetime/discovery. Each shmat opens the segment and
maps it MAP_SHARED at a chosen/requested address, then closes the descriptor.
The file remains until the final detach following IPC_RMID. This retains
file-cache/disk accounting rather than anonymous memfd accounting. A future
memfd implementation needs a surviving FD owner and an FD handoff protocol.

The arena, journal and segments use the existing locked 0700 tmpfs-cache
helper and collector (`vfs_temp_create` was extracted from vfs_mount).
Its reserved ownership FD survives fork, resident exec and payload FD sweeps;
stale directories are collected at a later loader start after every holder
exits. Put TMPDIR under the app's private cache on device. Creation failure
leaves normal payload startup available and IPC calls return the error.

Fork preparation reserves inherited attachments before clone. An inherited
flock ticket retains those reservations if the parent dies before the child
runs; the clone continuation registers ownership before returning to app code.

Semaphores support atomic multi-op/duplicate-op batches, GET/SETVAL and ALL,
wait counts and pids, IPC_STAT/SET/RMID, indexed STAT/STAT_ANY, IPC_INFO and
SEM_INFO, SEM_UNDO, and monotonic semtimedop deadlines. Blocking requests wait
on a shared sequence futex and periodically reap dead processes, including
zombies and reused PID numbers. SEM_UNDO belongs to the process, survives
exec, is not copied by fork and applies at exit_group, the last thread's
exit, or the next access after an unobserved death. A zombie thread-group
leader with other live threads does not prematurely release its undo.
Input operation/message buffers are copied before blocking. Kernel pipe
user-copy checks supply EFAULT handling without dereferencing app buffers.

Shared memory supports permissions, STAT/INFO, requested addresses, SHM_RND,
SHM_RDONLY, SHM_EXEC, whole-attachment SHM_REMAP, fork attachments, exec
detachment and removal at last detach. Message queues support typed,
negative-type and MSG_EXCEPT selection, zero-size messages, MSG_NOERROR,
queue-byte limits, blocking sends/receives, STAT/SET/INFO and removal wakes.
A receive with EFAULT consumes its selected message, as Linux does.
The reported configurable limits are Linux defaults: SEMMSL/SEMMNI 32000,
SEMMNS 1024000000, SEMOPM 500, SEMVMX/SEMAEM 32767, SHMMNI 4096,
SHMMAX/SHMALL ULONG_MAX-(1<<24), MSGMAX 8192, MSGMNB 16384 and MSGMNI 32000.
Backing-file offset/size and allocator arithmetic are overflow checked; normal
address-space, descriptor and cache-space exhaustion can still fail calls.

Remaining compatibility gaps:

- i386 payloads run under their own ELF32 proxy (static and dynamic, 64<->32
  exec handoff, dual-architecture filters, mixed-class SysV IPC; see "Round
  30 run 3" and "run 4" below). x32 is rejected. What i386 still lacks is
  outside the proxy: the in-process GPU forwarders and thread adoption are
  x86_64 only (32-bit GPU is planned via Mesa Venus to a 64-bit render
  server), and Flatpak's i386 compat extensions are untested end to end.
- SHM_LOCK/SHM_UNLOCK return ENOSYS, SHM_HUGETLB returns EOPNOTSUPP, MSG_COPY
  returns ENOSYS. SHM_INFO reports allocated pages and object count but does
  not reproduce resident/swap statistics.
- Partial overlap with another attachment in SHM_REMAP returns EOPNOTSUPP.
  Direct munmap/MAP_FIXED/mremap of an attached region is not reflected in
  attachment accounting. Use shmdt for the supported detach path.
- Undo sharing for process clones with CLONE_SYSVSEM, thread-local credential
  changes, supplementary-group/capability permission overrides, kernel wait
  fairness and full signal/restart semantics are not implemented. Permission
  checks use the real effective UID/GID; the proxy's virtual setup setters
  do not confer real credentials or capabilities. Nonlocal jumps/cancellation
  out of a nested app signal handler during an IPC transaction are unsupported.
- Journal snapshots traverse current metadata on each call. This favors
  simple crash recovery over high-object-count throughput. Recovery I/O failure
  is fail-closed; cache storage must remain accessible to the app.

Run `bash tests/sysvipc-host.sh` after build.sh, or `bash tests/run.sh` for the
full suite. Workload, edge, lifecycle and limit/ABI tests execute the real
proxy under the copied Android filter. They cover multi-process waits,
200-round semaphore ping-pong, killed undo owner/waiter, process/thread exit,
exec detachment and undo retention, objects created after fork, shared data,
last-detach removal, message queues, permissions, invalid pointers and a full
32000-semaphore set. A separate white-box recovery test kills a writer after
partial metadata mutations and checks rollback of values, arena growth,
object publication and records; that test runs without the copied filter.
Host tests and NDK r30/API-36 compilation pass. No adb, image build, VM,
Steam or enforcing device-policy validation is claimed.

For a coordinator-built fresh image on the Ryzen PC, Surface Pro 3 or ProDesk:
run the four standalone workload binaries through the packaged loader with
the app UID and a private TMPDIR, omit --android-filter when the process
already inherits that filter, and collect stdout/stderr. Check that normal
Flatpak startup still works, sibling processes observe newly created objects,
SIGKILL releases undo on the next operation, and relaunch collects abandoned
cache directories. Test native x86_64 clients first; a 32-bit Steam launch is
not a valid success criterion until the separate i386 loader work lands.
No kernel configuration or AOSP change is required for the x86_64 path.

### Round 24: Rio worker-thread startup crash

The merged loader reproduces Rio's exact `pc=0x600000009940` fault in a
host worker-thread stress test. In the **matching** `bea4eb9` NDK binary this
is `p_equal`, in `.ltext`, reading a freed VFS cache key. Comparing that PC
with a subsequently rebuilt loader misidentifies it as outside text. The
shared output binary can be replaced by other builds; use an isolated
`MATON_V3_ANDROID_OUT` when diagnosing addresses.

The 16-entry mount cache had unsynchronized readers, key replacement/free,
and separate publication of key and mount identity. Worker SIGSYS handlers
could read unmapped strings or receive a different mount identity (`EXDEV`,
or even `EROFS` on a writable bind). The cache is removed; resolution walks
the small mount list. The same stress also exposed freed fd-name list nodes;
short signal-masked critical sections now protect fd-name lookup/copy,
publication and removal. They never span a payload syscall or blocking flock,
and a fork child can discard the inherited parent's lock owner.

This is an existing race exposed during the HostCommand merge, not evidence
of bad HostCommand fd remapping, TLS, or server setup: the pre-merge
`fd37033` loader also crashes in `p_equal` under concurrent stress, and the
merged loader with HostCommand disabled also returns wrong mount errors.
The exact timing change that made Rio trigger it has not been isolated.

`tests/host-command-threads.py` runs eight workers through 8,000 explicit
openat(O_CREAT)/ENOENT, openat, rename, flock, close and unlink sequences
under the copied Android filter, after bwrap's first resident exec with
`/.flatpak-info` present. An idle socketpair-backed HostCommand worker proves
the snapshot ran; no command is requested. The test is part of `tests/run.sh`.
To check Android code generation on the host, pass
`--android-compiler <NDK>/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android36-clang`.

Validation: full `tests/run.sh` and `build-android.sh` passed. Repeated NDK
stress and production-listener runs passed; original merged production runs
crashed at the exact Rio PC four times, and pre-merge runs also crashed.
No adb or image build was used. Device confirmation remains: build/package
this loader in the host APK and a fresh image, boot normally on each target
PC, launch Rio repeatedly, exercise new tabs and shader-cache creation, and
check that it stays alive and its terminal commands work. Check ETR as well.
No shared-file, AOSP, compositor or kernel changes are required. This fixes
startup file traffic; concurrent mount/root mutation and other proxy state
are outside this regression's coverage.

Round 25 resolver: the stub maps its per-app resolver at both
`/etc/resolv.conf` and `/system/etc/resolv.conf`. On the based-on-AOSP host,
`/etc` points to `/system/etc`; the VFS follows ancestor symlinks before
selecting leaf mounts. The old first map alone was invisible to Flatpak's
`g_file_test` fallback after the broker rejected SessionHelper RequestSession.
Stock Flatpak now finds and read-only binds the file; no Flatpak patch or
SessionHelper emulation is needed. HostCommand forks the completed app root,
including its resolver bind; DNS helper state is process-local and starts lazily.

Stock Flatpak constructs resolver binds even with `--unshare-net`. At a
successful payload exec in a Flatpak root (`/.flatpak-info`), the proxy hides
`/etc/resolv.conf` and `/run/host/monitor/resolv.conf` for virtual NEWNET.
Construction is unaffected; the existing network filter continues to deny
connections. This state also reaches HostCommand children.

Validation: resolver bind mapping and NEWNET hiding are covered by the
existing VFS tests. Host/NDK builds and `tests/run.sh` pass. SDK-36 javac
compilation of V3Launcher with dependency fixtures passes. Device validation is pending: see
`out/pc-logs/agents/v3-resolv-result.md` for fresh-image steps. No kernel,
SELinux or shared-file changes are required.

### Round 30: i386 design and P1 gate (support not implemented)

*Historical: written before i386 support landed. The filter and loader gaps
described here were closed in runs 3 and 4 below (abd18d8, 8a9e02d); the design
notes remain as reference.*

P0 chooses a separate freestanding ELF32/EM_386 resident proxy, sharing C
algorithms with the ELF64 build through explicit ABI adapters. Build it with
host `-m32` and NDK `i686-linux-android36-clang`, without a libc dependency.
The resident loader must use Elf32 headers/program headers/auxv, 32-bit stack
words, an i686 platform string and a reserved low-address resident region.
Reject segment overflow, proxy/stack collisions and invalid interpreter
class/machine. An address must fit in 32 bits before converting it. The
existing 0x600000000000 placement and large code model cannot be reused.

Option (b), merely dispatching i386 syscall numbers in the existing 64-bit
handler, is ruled out for this implementation: it does not enter compat code,
build compat signal frames, supply TLS or fix pointers. This is not a claim
that mixed-mode x86 execution is physically impossible: a 64-bit executable
can issue int 0x80, and deliberate mode-switch machinery is a different,
substantially more complicated design. Kernel exec of an ELF32 proxy gives
normal compat process startup, signal frames and a compat vDSO.

Two filters must be considered independently. The checked-out
`bionic/libc/seccomp/seccomp_policy.cpp` selects AUDIT_ARCH_X86_64 and
AUDIT_ARCH_I386 with separate app allowlists. Our host `--android-filter`
currently copies only x86_64 and kills other architectures; it is **not** a
correct dual-architecture reproduction. Generate an x86 allowlist from the
same AOSP inputs and unistd_32.h, retain input hashes, and test both branches.
The proxy's own filter also currently kills I386. It must dispatch by arch
before interpreting numbers and reject x32. Binder ioctls are ordinary
emulated syscalls and are not filtered as a security policy. Trap payload operations needing
VFS/IPC/socket conversion; handler replay must use only calls allowed by the
inherited policy. An ALLOW in our filter cannot override Android's TRAP.

For i386 raw calls use EAX for the number and EBX/ECX/EDX/ESI/EDI/EBP for
arguments. Preserve callee-saved registers. A separate int 0x80 gateway and
clone continuation are required. The i386 rt_sigaction wire structure needs
an eight-byte kernel signal mask (a single 32-bit unsigned long is too small).
Use i386 ucontext REG_EAX/EBX/ECX/EDX/ESI/EDI/EBP, REG_EIP and REG_ESP;
rt_sigreturn is syscall 173, not x86_64's 15. Restore the complete compat
signal/FP state and handle nested signals/alternate stacks. Do not assume
that every trapped entry ends in a two-byte instruction to rewind. int 0x80
and the compat vDSO __kernel_vsyscall sysenter path both need tests: the
latter has its own saved-stack/return convention. Omitting AT_SYSINFO is an
initial int-0x80-only strategy, not evidence that sysenter works.

set_thread_area manages compat TLS descriptors (struct user_desc) and glibc
uses GS. Do not substitute arch_prctl or clear the application's TLS on
handler entry. The handler must remain freestanding and not use compiler TLS.
clone(CLONE_SETTLS) passes an i386 descriptor pointer; clone argument order
and child-stack continuation need their own gateway. Test pthread creation,
futexes, robust lists, signal delivery and fork while TLS is active in P2.

ABI adapters are mandatory, including for internal raw syscalls:

- mmap2 uses 4096-byte offset units; byte offsets must be aligned and checked
  before shifting. Old mmap(90) takes an argument block, not six registers.
  i386 pread64/pwrite64 split 64-bit offsets across words. ftruncate64 and
  other wide arguments likewise cannot use the current long-only wrapper.
- stat/stat64/fstatat64 need explicit kernel wire layouts. NDK bionic
  `struct stat` is not automatically the i386 kernel stat64 buffer. statx
  uses fixed-width fields but still needs size/offset assertions and careful
  payload copies. Internal VFS and link bookkeeping need canonical metadata.
- sockaddr layouts generally share fixed fields, but msghdr, iovec and cmsghdr
  have 32-bit pointers/size words/alignment. socketcall(102) decodes an
  argument-word array; direct socket calls must use the same DNS endpoint rewrite.
  Convert cmsg lengths/alignment and preserve SCM_RIGHTS private-fd guards.
- Legacy timespec/timeval have 32-bit seconds; time64 syscalls use 64-bit
  seconds and nanoseconds in their kernel structures. Convert explicitly,
  range-check legacy outputs and cover futex/ppoll/pselect/clock and timeout
  variants. Native bionic time_t width is not a wire-layout specification.
- Classic termios and termios2 require verified wire sizes/ioctl encodings;
  reuse the safe kernel-mediated copy and existing BOTHER behavior, not a
  fabricated tty result.
- ipc(117) needs operation/version decoding, IPC_64 command handling, the old
  msgrcv argument block and shmat result through its output pointer. Direct
  calls 393–402 and semtimedop_time64(420) use the same emulated namespace.
  Convert semid64_ds/shmid64_ds/msqid64_ds, info structures, message long types
  and high/low time fields; validate narrowing and inaccessible pointers.

Cross-class exec is a P3 requirement, not an automatic consequence of having
two binaries. Existing VFS lists and private-fd/trace state reside in pointers;
kernel exec destroys them. Serialize into an authenticated private inherited
FD using fixed-width records/offsets, bounds checks and a version. Carry root,
cwd, mounts, fd names, link/temp ownership, DNS/network state, diagnostics and
IPC namespace identity; preserve payload CLOEXEC semantics and reclaim failed
handoffs. Deserialize before payload loading and reinstall compat signal
handlers. IPC shared arena records currently contain unsigned long, long,
size_t and architecture-dependent ipc64_perm: these need a fixed-width common
format before the two loaders can safely share a namespace. Carry attachment
cleanup and SEM_UNDO process identity through exec, not just the arena fd.

Seccomp filters survive kernel exec and cannot be loosened. Therefore a
cross-class handoff cannot start with today's architecture-killing proxy
filter and then install a new one. Both arch branches and bootstrap/raw
instruction locations must be designed into the initial filter; simply
stacking new per-loader IP exceptions leaves the older filter trapping them.
A fixed-address bootstrap with a stable, explicitly permitted gateway is one
candidate, but needs an actual inherited-filter test before acceptance. Exec
    must not expose a payload bypass around private-fd checks. The compat
    proxy is an emulation layer, not a security boundary; Binder access is
    controlled by the stub app's UID and SELinux domain.
See [seccomp semantics](https://man7.org/linux/man-pages/man2/seccomp.2.html)
and [x86 kernel entries](https://www.kernel.org/doc/html/v5.10/x86/entry_64.html).
No kernel implementation code is used here.

P1 is **failed**, so P2/P3 were not attempted. `bash tests/run.sh --i386-p1`
builds an original static ELF32 fixture and first requires native compat
execution. In this agent runner that prerequisite dies with SIGSYS (even a
separate int-0x80 exit-only ELF32 probe dies with SIGSYS). Native fixture
execution therefore has not passed. On a compat-capable unrestricted host
the next gate requires stat64/open/read/write through a virtual leaf map
with `--android-filter`. Separately invoking the current proxy with this
ELF32 fixture returns ENOEXEC before filter installation. This is a failure
gate, not a passing i386 regression. Normal
`tests/run.sh` remains the x86_64 suite. `MATON_V3_I386_LOADER` can select a
future standalone ELF32 loader for the same gate. A direct NDK i686 build
probe also fails on the current architecture-specific source; changing the
compiler target is insufficient. Neither build script produces an i386
proxy, and no second loader was packaged into the APK. Future packaging must
place the compat loader alongside the extracted x86_64 loader (the host APK
remains x86_64); class handoff must use that trusted installed path, not an
unverified environment-provided executable. Do not use Steam as a success
criterion until P1–P3 actually pass.

#### Portability contract for the next implementation

This is a design contract, **not an implemented arch abstraction**. The
existing core still uses native SYS_* numbers and native libc/kernel
structures. Moving raw.S alone would not satisfy the portability requirement.
The current runner fails the native ELF32 prerequisite, so no unverified
backend or partially converted core is introduced in this round.

Before implementing i386, refactor and verify x86_64 with this boundary:

- `arch/arch.h` documents a common syscall enum independent of kernel
  numbers, six normalized argument words, canonical signed/unsigned 64-bit
  values, and an explicit payload pointer type. Decode a payload number into
  a common operation plus a wire-ABI variant; encode an internal raw operation
  into the selected kernel ABI. Unknown calls remain distinguishable from
  known emulated operations and receive the existing ENOSYS policy.
- Build selection chooses `arch/x86_64/` or `arch/i386/`; each supplies the
  number mapping, raw/entry/sigreturn/clone assembly, ELF class/machine and
  auxv/stack construction, resident address bounds, audit architecture,
  gateway addresses, signal-context get/set hooks, TLS setup/reset hooks and
  trap restart handling. Core code must not index REG_* or select arch macros.
  Bootstrap and inherited seccomp policy are part of this boundary, including
  cross-class exec gateways; they cannot be repaired after filter installation.
- Wire adapters decode/encode stat, signal actions/masks, socket messages,
  ancillary data, iovecs, legacy/time64 timeouts, termios and IPC structures.
  Native headers may describe a backend wire format only after size/offset
  assertions. Core VFS/link/DNS/IPC routines operate on canonical values;
  payload buffer copies retain EFAULT handling and reject narrowing overflow.
  IPC message types use signed canonical values, never a host-sized long.
- Shared IPC arena and exec-state formats use fixed-width fields and offsets,
  an explicit format version, bounds checks and no process pointers. Preserve
  the namespace, SEM_UNDO identity, ownership/private descriptors, fd aliases,
  CLOEXEC semantics, diagnostics, root/cwd/mount state and network state.
  The existing shared arena is not yet safe for mixed-class processes.

To add arm64 or riscv64 later, implement those same backend hooks and tables,
select them in both build scripts, generate their checked-out AOSP app policy,
and add native static/dynamic, signals/TLS/thread, wire-layout, VFS/socket/IPC
and inherited-filter exec tests. Do not implement either backend now. Mapping
syscall names is insufficient where an architecture has a different calling
convention, a multiplexer or a different structure representation.

Acceptance order remains: all existing x86_64 tests after its full interface
refactor; i386 P1 static VFS under the copied policy; P2 dynamic runtime and
threads/signals/socket conversions; P3 bidirectional resident exec and shared
IPC state. Tests must exercise actual binaries, not only mapping tables.
The phase-stop prerequisite failure does not waive the x86_64 refactor or any
of those gates for a future implementation.


### Round 30 unrestricted rerun: partial architecture extraction

*Historical: the i386 loader and P2/P3 were implemented in runs 3 and 4 below.*

The earlier runner failure above is superseded: native ELF32 exit and
stat64/open/read/write fixtures now pass without the Codex sandbox. The P1
proxy gate still fails with ENOEXEC. No i386 loader is implemented; P2 and P3
have not been attempted. This is partial work, not i386 payload support.

The x86_64 loader now dispatches common `PROXY_SC_*` operation IDs rather
than native `SYS_*` numbers. `arch/arch.h` selects `arch/x86_64/backend.h`;
`arch/x86_64/syscalls.h` maps between those IDs and kernel numbers. `RAW`
encodes once at kernel entry. SIGSYS decodes before core dispatch. Seccomp
comparisons and copied-policy lookups explicitly encode their operands.
Unknown numbers and x32 do not become known internal operations. These IDs
are process-local and are not an exec-state or shared-memory format.
`gen-arch-syscalls.py` regenerates the checked-in tables from the local AOSP
UAPI specification. It does not copy kernel implementation code.

The backend also owns ucontext register indices, syscall restart instruction
size, kernel signal-action layout, TLS clearing, ELF types/class/machine,
platform string, resident address bounds, audit architecture and assembly.
The top-level assembly files retain include wrappers for existing fixture
builds; production build scripts compile the backend files directly.
`tests/arch-map.c` verifies the mapping boundary, while the full actual
x86_64 payload suite verifies behavior, including real AF_UNIX DNS binds.

The mandatory portability refactor is **not complete**: stat and socket
message layouts, time structures, termios conversion, IPC wire structures,
shared IPC records, signal-frame handling beyond register access, and
seccomp gateway construction still need backend adapters and fixed-width
canonical representations. Core syscall arguments are still native `long`.
Do not select an i386 compiler and treat the extracted hooks as sufficient.
The build scripts still produce only the x86_64 proxy, and APK packaging is
unchanged. A future backend must implement these remaining interfaces before
its native static/dynamic and inherited-filter exec gates can pass. The
original P0 design and acceptance order above still apply.

### Round 30 run 3: ELF32 resident loader and compat fixtures

This supersedes the earlier P1 ENOEXEC result. `build.sh` now produces
`build/linux-layer-v3-i386`; `build-android.sh` also produces
`libmatonos-v3-loader-i386.so`. Both are freestanding ELF32/EM_386 binaries
without a libc interpreter. `build-i386.sh` uses NDK r30/API 36 with `-m32`;
the NDK supplies headers on hosts without multilib development headers and
compiler builtins for signed 64-bit division. No upstream source is vendored.
The host's glibc 2.39 i386 runtime is used only by dynamic test payloads.

The selected i386 backend owns raw int-0x80/entry/rt-sigreturn/clone assembly,
ELF32 headers and stack words, its 0x70000000 resident range, ucontext slots,
GS reset across resident exec, and syscall mappings. The loader preserves
AT_SYSINFO and AT_SYSINFO_EHDR: actual glibc vDSO calls, infinite-poll retry,
TLS (`set_thread_area`), pthread/futex operations and signals are tested.
A six-word `proxy_arg_t` array is always signed 64-bit. Kernel replay converts
mmap byte offsets to mmap2 page units, splits pread64/pwrite64/ftruncate64
arguments, and uses _llseek for internal seeks. Compat socketcall(102) and
ipc(117) decode in the backend before entering common algorithms. `ipc(117)`
handles semctl's indirect union argument, version-zero msgrcv's kludge, and
shmat's output pointer. Direct IPC control commands strip IPC_64, and syscall
420 reaches the common semaphore engine with an explicit time64 timeout.

VFS/inode/IPC internal metadata now uses `proxy_stat`. stat/statx,
msghdr/iovec/cmsg/message batches, timeouts and IPC control wire access is in
`arch/` adapters; size/offset assertions verify the selected compiler layouts.
Termios2 uses a fixed-width 44-byte x86 wire type. Kernel signal masks are
always eight bytes, including short bookkeeping locks; context masks and
altstack operations use backend hooks. Raw/clone BPF PCs are supplied by
backend hooks rather than shifting a 32-bit uintptr_t by 32 in the core.
Shared IPC v2 header/object/record/message sizes are explicitly asserted as
72/144/56/32 bytes on both builds. Those records use fixed-width fields,
64-bit message types and times, and explicit eight-byte alignment. User IPC
control buffers still use the selected kernel wire form and split time highs.

Verification commands:

```sh
bash tests/run.sh --i386-p1
bash tests/run.sh --i386-p2
bash tests/run.sh
MATON_V3_ANDROID_OUT="$PWD/build/android" bash build-android.sh
```

The default suite includes the compat fixtures. P1 passes mapped static
stat64/open/read/write. Dynamic fixtures pass actual glibc startup, twelve
thread cycles before and after same-class resident exec, TLS isolation,
futex waits, infinite poll, getdents64, signals, sockets and SCM_RIGHTS, and
DNS sendmsg/recvmsg conversion over a real Unix transport. Additional compat
fixtures pass pty/termios2, ipc(117) semaphores/shared memory, direct semget/
semctl IPC_STAT, and a syscall-420 timeout. Native fixture execution is checked
separately; SysV fixtures remove the host kernel objects they create.
`MATON_V3_I386_LIBC` and `MATON_V3_I386_INTERPRETER` select another existing
compat glibc runtime. No downloading, package installation, adb, image build,
APK update, or device-policy validation is involved.

**P3 is not implemented.** A cross-class resident exec still returns ENOEXEC.
Each copied app filter and proxy filter accepts its selected architecture
only. A dual-architecture inherited filter with stable bootstrap/raw gateways
and a private authenticated, versioned FD handoff still must be implemented
and tested. No mount/fd/DNS/inode/IPC namespace serializer or peer-loader path
is present. Fixed-width IPC records alone do not connect independently
started loaders to the same namespace. No second loader was packaged into
linux/subsystem/host: ship it alongside the trusted extracted loader only
once that selection/handoff protocol works. This is missing implementation,
not an external blocker, and does not establish Steam/Wine support.

The full portability contract is also unfinished: legacy stat(106)/lstat(107)/
fstat(108), old mmap argument blocks, remaining legacy/time64 structure
translations and IPC info-buffer adapters need coverage. Some common legacy
conversions still use native timespec/timeval/fd-set structures. The time64
adapter currently covers semaphore deadlines, not every time64 operation.
DNS integration covers message conversion, not complete i386 getaddrinfo/TCP/
timeout/resolver behavior. IPC message queues, fork/exec namespace inheritance
and full compat signal/FP-frame edge cases need broader actual-binary tests.
Keep arm64/riscv64 work behind the same backend interfaces; do not infer that
matching syscall tables alone makes either supported.

Device acceptance remains pending. After completing P3, the coordinator must
package both loaders and build a fresh image. On Ryzen/RX 6600, Surface Pro 3
and ProDesk, launch the static/dynamic fixtures as the actual app UID without
--android-filter, verify inherited seccomp and both exec directions, check
VFS/private/diagnostic descriptors and a shared mixed-class IPC namespace,
then exercise real Steam/Wine workloads. No kernel or AOSP change is requested.

### Round 30 run 4: bidirectional resident exec (host P3)

This supersedes the run-3 ENOEXEC and single-architecture filter limitations.
The original freestanding x86_64/i386 loaders now hand off in both directions.
`tests/run.sh` includes actual mixed-class processes, inherited filters, shared
IPC, DNS, FP state, and NDK artifact/extraction tests. Image acceptance is still
pending the two coordinator edits in [SHARED-CHANGES.md](SHARED-CHANGES.md).
No AOSP, kernel, adb, image or git metadata changes were made.

A cross-class ELF causes a kernel exec of a verified sibling loader next to
`/proc/self/exe`. Selection uses backend class/machine hooks, never an
application-controlled environment path. The sibling must be a regular,
executable, non-symlink file with the running loader's owner, in a directory
without group/other write permission. It is opened once and executed through
`execveat(AT_EMPTY_PATH)` to avoid a replacement between checking and execution.
The production names are `libmatonos-v3-loader.so` and
`libmatonos-v3-loader-i386.so`; host fixture builds use `linux-layer-v3` and
`linux-layer-v3-i386`. Both scripts produce trusted 0755 executable files.

The private handoff is an anonymous memfd with all four immutability seals.
Its version-1 stream has fixed-width 64-bit words (little-endian x86 ABI),
length-prefixed strings, bounded counts, a 64 MiB total limit, an exact end
check, format magic, process PID and a getrandom-generated bearer nonce.
The trusted peer must present the matching nonce; wrong PID/version/nonce,
unsealed, oversized or truncated streams fail closed. Import closes the FD
and erases the FD/nonce arguments from the original kernel cmdline before
payload entry. This authenticates the private handoff within the existing
app-UID trust boundary; it is not a signing authority or a sandbox boundary
between mutually hostile programs with the same UID.

State includes root/cwd/executable, ordered mount sources/targets/flags, fd
names, private directory-flock ownership, GPU publication source paths,
CLOEXEC-aware descriptor aliases, the diagnostic sink, virtual namespace and
user-namespace-limit state, HostCommand configuration, DNS endpoint/path,
connected socket aliases and epoll watches, and virtual netlink endpoints,
aliases and queued replies. `state.c` owns framing; each subsystem owns its
state adapter. No process pointers or native `long` values cross the stream.
Hidden inode ownership uses a fixed-layout, shared memfd mapping so forked
processes keep the same refcounts through exec; the backing FD is private.
Closed CLOEXEC aliases drop their ownership only after all aliases have been
imported. Persistent hardlink count sidecars are now eight bytes on both ABIs.
Payload read/write/vector I/O, mmap, truncate, seek and metadata calls cannot
use private ownership/diagnostic/state descriptors through normal syscall
entry; close/dup/fcntl protections remain. Binder ioctls follow normal proxy
routing and are not filtered as a security policy.

The IPC arena remains the shared fixed-width version-2 format from run 3.
Handoff carries its namespace paths and the inherited directory lock, and
reopens the same arena. Kernel exec keeps the process PID/birth identity,
therefore SEM_UNDO remains attached to that process rather than being applied
at exec. Import removes that process's old attachment accounting without
unmapping the old addresses in the new loader; removed segments are reclaimed
when the last attachment disappears. Normal exit still applies undo. Tests
use a forked parent/child of opposite classes sharing semaphores, shared memory
and queues, then exec back and check the child's undo after exit.

Both filter branches are installed from the outset. `arch/x86-filter.h`
contains the x86-family architecture selection, syscall-number
translation and gateway exemptions. The copied AOSP app policy includes both
allowlists from the same checked-out inputs and records their hashes. Both
number tables use fixed ABI numbers even when compiled by the other backend;
using compiler-native SYS_* values for the peer was incorrect. The raw and
clone gateways have fixed linker sections with return PCs at +0x42/+0x102:
`0x600000300000` for x86_64 and `0x70300000` for i386. Bootstrap and inherited
filters recognize the peer before another filter is stacked. Unknown audit
architectures and x32 still fail closed. Each class has its own signal/FP,
TLS and raw entry ABI; the core remains common-ID C.

To add another architecture, implement its number tables, raw/entry/clone/
sigreturn gateways, ELF/ucontext/TLS and wire adapters, peer class/name hooks
(`arch/exec.h`): declare `ARCH_ELF_CLASS`/`ARCH_ELF_MACHINE` and
`ARCH_PEER_ELF_CLASS`/`ARCH_PEER_ELF_MACHINE` in the backend, with the peer
loader names. `arch_exec_class()` in `arch/exec.h` classifies these pairs for
both exec preflight and handoff. Also implement an inherited filter backend selected through
`ARCH_FILTER_HEADER`. Add both AOSP policy branches and stable gateway PCs
before permitting class handoff. Extend both build scripts and actual static,
dynamic, signals/FP/TLS, fd/VFS, socket/IPC and bidirectional exec fixtures.
Do not insert audit IDs or ELF machine conditionals into core algorithms.
Arm64 and riscv64 are not implemented.

Validation in this worktree:

```sh
bash tests/run.sh
MATON_V3_ANDROID_OUT="$PWD/build/android-r4" bash build-android.sh
MATON_BUILD_COORDINATOR=1 bash ../../tools/preflight.sh
```

The full suite passes, including both directions under inherited dual filters,
a real x86_64 bwrap launching i386, mapped files, dup/CLOEXEC/trace ownership,
hidden inode aliases, exec detach of removed shared memory, mixed-class
sem/shm/msg/SEM_UNDO, active DNS aliases/epoll and preserved custom argv[0].
Compat message tests exercise old ipc(117) receive blocks, E2BIG without
consumption, NOWAIT and direct controls. FP tests exercise x87 and SSE across
SIGSYS and application signal delivery. Real glibc getaddrinfo tests cover UDP,
truncated UDP -> TCP and a bounded timeout over Unix sockets. These use the
installed glibc 2.39 only as a fixture runtime, not as a shipped dependency.
Preflight still fails on missing pre-existing image APK/native artifacts in
this isolated worktree; no AOSP build was requested.

Gradle now declares and copies both NDK executables to the APK's x86_64 JNI
directory and keeps both unstripped. The i386 file intentionally lives beside
the x86_64 one because the host app remains x86_64. `extract-loaders.sh` extracts
both entries with 0755 permissions. Tests use real NDK executables in a small
ZIP to check the extraction contract, then run those APK-named siblings under
the inherited filter. This is not a signed host-APK or device test.
`tools/build-apps.sh` and `buildinfra/buildinfra.mk` currently stage/install
only the ELF64 copy: the coordinator must make the exact two shared changes
before image packaging is complete.

Remaining acceptance: the coordinator builds the signed host APK and a fresh
image, verifies both peers in the actual nativeLibraryDir, then runs the same
fixtures as the app UID under the actual inherited app policy (without
--android-filter) on Ryzen/RX 6600, Surface Pro 3 and ProDesk. Re-run both exec
directions, bwrap, shared IPC/undo, TLS/threads/signals, active resolver and
private descriptor tests. Check missing/corrupt peer rejection and that
ordinary boot/app startup is unaffected. Steam/Wine acceptance is not claimed.
Legacy stat(106–108), old mmap argument blocks and the remaining time64/IPC-info
wire variants from run 3 still need their separate portability coverage.

### APK pack mounts (host fixture and trusted launcher API)

```
build/linux-layer-v3 --android-filter \
  --pack-mount /packed /trusted/bundle/objects.index /trusted/bundle \
  /packed/program
python3 tests/pack-host.py
```

`pack_mount(target, index_path, pack_directory)` is the corresponding trusted
launcher setup API. Mounts can be repeated. The launcher supplies all three
arguments before payload entry; payload mount syscalls cannot create a pack
mount. The index is loaded with the caller-buffer reader in `../packs/`, and
referenced immutable APK-style ZIP files are opened read-only and pinned.
No checkout, extraction of file payloads, kernel mount, or AOSP change occurs.
Pack state is specific to this VFS feature, not a general virtual-fd layer.

Opening an indexed regular file returns a real descriptor plus its indexed
slice metadata. A new open reopens the pinned carrier through procfs before
`F_DUPFD*`: duplicating the mount's one descriptor directly would incorrectly
share a position between independent opens. Actual dup aliases, including
aliases inherited through fork, share their position through the kernel.
CLOEXEC, close/close_range and descriptor replacement update the slice table.
Directory descriptors duplicate a pinned, empty, unlinked directory carrier;
getdents64 synthesizes index children, with kernel-backed seek cookies shared
by dup/fork aliases. Relative openat, chdir/fchdir and getcwd retain virtual
names. Proc self/current-pid fd readlink exposes those names.

Reads and vector reads clamp to the indexed EOF; positional I/O and mmap
translate logical offsets to APK offsets with overflow checks. Vector offset
encoding, iovec access and stat/statx output stay at the architecture boundary.
Writable shared mappings fail with EACCES; file writes fail with EBADF.
Indexed path metadata, access checks, symlinks and directory entries are
synthetic; mutations return EROFS, including host-source rename/link into a
pack. sendfile, copy_file_range and splice sources use chunked clamped reads;
the chunk size does not limit the transfer length. Synthetic directories have
size zero, nlink one and no stored timestamps; regular and symlink sizes,
modes, uid/gid and checksum-derived inode identities come from the index.

Same-class resident exec retains pack state. Peer-loader exec carries the
index bytes, private carrier descriptors and surviving slice identities in
the sealed version-2 state stream; payload CLOEXEC aliases are omitted.
Both trusted siblings must be rebuilt together after this stream change.
The existing VFS concurrency, user-pointer and containment limitations above
also apply here. A small shared per-mount lock serializes each position
snapshot/clamp/read or seek across threads, fork and peer exec, so simultaneous
alias reads cannot run into the next APK entry. Kernel descriptors continue
to own position sharing; this is not a general virtual-fd implementation.

**SCM_RIGHTS of pack descriptors is out of scope.** The real kernel descriptor
can be transferred, but its index/slice metadata is not registered in the
recipient. Do not send pack descriptors through SCM_RIGHTS expecting virtual
file semantics; the recipient would see the underlying APK descriptor.

`tests/pack-host.py` creates a real bare-user OSTree repository and uses
`linux/packs/pack.py` to produce page-aligned STORED ZIPs. Under the copied
Android filter it executes packed static/dynamic binaries and a shebang,
loads a packed shared library through ld.so, compares cat/sha256sum/dd output
(including a >1 MiB payload), and compares ls -lnR metadata against a real
OSTree checkout. Its C/ELF32 fixtures exercise relative directories, vector
EOF, offsets, mmap, transfers, dup/fcntl/dup2/dup3, fork, same-class exec,
x86_64 -> i386 -> x86_64 sealed handoff, CLOEXEC/close_range and EROFS.
The standard `tests/run.sh` includes this acceptance test. No VM or AOSP build
is needed; these are host compatibility tests, not device acceptance.

ELF PT_LOAD segments now use private file mappings, including APK carrier
slices, when page-congruent. BSS tails are COW-zeroed and full BSS pages are
anonymous. Permission refusals (EACCES/EPERM) retry through a writable anonymous
copy followed by the requested protection; noncongruent ELF segments retain
the copy path. Intercepted executable file mmaps use the same refusal retry.
Read-only shared requests fall back to private copies; writable shared requests
never do. A fallback diagnostic is emitted once per backing inode/slice in the
resident process. Copies do not track later file changes or emulate SIGBUS on
pages beyond EOF. Normal successful mappings retain kernel file semantics.
`tests/loader-maps.py` inspects two live processes' real maps/smaps for clean
file-backed text and uses kernel seccomp EACCES/EPERM injection for both loader
classes, resident exec, dynamic linker mmaps and pack-backed main binaries.

Incremental hardlink counts (2026-10-17): ordinary link, unlink and rename
transactions never enumerate the store or walk its tree. The existing
`i-*.count` format stays an unsigned 64-bit native-endian value (both supported
x86 backends are little endian). First conversion writes count 1; each later
transaction applies its namespace delta directly. The unlink that reaches zero
removes the object and sidecar; open descriptors still report zero links.

A store's existing exclusive `flock` covers recovery, intent publication,
namespace mutation, count writes and garbage collection. No second store lock
is acquired. `intent` is a 16,520-byte v2 record shared by x86_64/i386: uint64
magic `0x76336c696e6b0002`, two baseline counts, two baseline name-presence
counts, two PATH_MAX object strings, and two PATH_MAX name strings, followed by two 48-byte canonical physical
placeholder identities used to retire overwritten/unlinked index entries. Empty
object slots are unused. Both names are inspected for each affected object;
replay writes `baseline_count - baseline_presence + current_presence`.
This also covers overwrite/exchange and failed syscalls, and is idempotent
when interrupted during a count write or garbage collection. Duplicate names
and objects are counted once. Arithmetic checks uint64 overflow/underflow.

Intents are completely written to `pending`, then atomically renamed to
`intent` before changing names. An interrupted first conversion uses the
existing NUL-separated source/object `conversion` journal, now published by
the same mechanism; recovery restores the source or writes count 1 using
only those paths. A leftover `pending` has not authorized any mutation and
is discarded. `intent` is removed only after count updates and collection.
Legacy `dirty`, invalid intent records, and missing/short/zero count metadata
are exceptional recovery cases. They permit one complete recount attempt per
store inode/device per process. A failed attempt or repeated damage returns
an error instead of repeating the walk. Healthy reads only read metadata;
ordinary operations never create `dirty`. Old symlink-format stores are not supported. Do not mix old and new writers on a live store: old writers still
use the expensive dirty protocol. As before, the guarantee concerns process
interruption, not power loss: there is no fsync durability protocol. Direct
host mutations bypassing the proxy remain unsupported. Structurally valid
but incorrect external count edits cannot be detected without recounting.

`tests/links-incremental.sh` builds a test-only loader. Its recount diagnostic
counts directory visits; its `stop-intent` hook stops replay after intent
publication and namespace mutation, before count updates. Neither hook is
present in production builds. The test creates 20,000 files, adds 5,000
emulated links in another directory, and compares 25,000 stats to the same
proxy's plain-file baseline (required ratio <5). It requires zero tree walks
for these mutations/stats and SIGKILL recovery, checks that an unrelated
object's sidecar bytes/mtime did not change, and covers overwrite/exchange,
failed publication, same-name NOREPLACE, immediate zero-count collection and
one-shot corrupt metadata fallback. It is included in `tests/run.sh`.

Stored hardlink names and no-follow lookups (2026-10-17): `links_target()`
recognizes the existing `.v3-inodes` representation and substitutes only the
final backing regular file. Open and at-style flags remain intact; real
symlinks retain kernel behavior. Empty-path stat calls reuse the existing fd
link-count path, and readlink is passed directly to the kernel.
Path-first xattr operations, the newer xattrat family, and inotify's
IN_DONT_FOLLOW go through the same translation. No additional tree walks
are introduced. `openat2` remains ENOSYS.

`tests/hardlinks.c` covers the libglnx O_PATH/no-follow/statx/reopen sequence,
metadata and xattr variants, real symlinks, and no-follow inotify watches.
`tests/i386-nofollow.c` covers the ELF32 statx/readlinkat layouts. Run the full
suite with `bash linux/compat-proxy/tests/run.sh`. To also compile and run the
actual upstream libglnx chase implementation, set `MATON_TEST_GLNX_SOURCE` to
an existing libglnx source directory; this optional test requires host GLib
and gio-unix development packages. All optional-test outputs stay in build/.

The inherited app syscall policy remains authoritative: on this checkout
`name_to_handle_at` is denied (ENOSYS); its path route is ready if permitted,
but does not bypass that denial. Host filesystems/kernels without xattrat
return ENOSYS. Validation is on the host under the copied app filter, not a
fresh MatonOS VM deployment; rerun Flatpak installation on the rebuilt image.

### Packed directory descriptors in bwrap (2026-10-19)

A packed directory's real descriptor refers to an empty, unlinked carrier.
Resolving `/proc/self/fd/N` through the kernel's readlink text therefore lost
the indexed source when bwrap used that descriptor as a bind source. The host
regression on the original code reports `packed fd bind changed inode`; the
packaged bwrap reports `Race condition binding dirfd` before executing
ldconfig. Proc magic links for tracked packed directory descriptors follow their
recorded VFS names, including the rebasing already performed by pivot_root.
The normal pack lookup then supplies the source directory and its identity.

Trace mode records each path call's syscall/dirfd, original path and resolved
path before pack dispatch, alongside the existing bwrap argv/bundle, mount,
exec and syscall-result diagnostics. Flatpak passes the cache subprocess's
stdout/stderr through to the launcher.

`tests/pack-host.py` checks source/destination inode equality, dirfd-relative
lookups of existing GL parents, their EEXIST results, file contents, and EROFS
plus ENOENT when trying to create a missing directory. When a static host
ldconfig and bwrap are available, it executes the packed stock ldconfig with
`-X -C`, using runtime/cache-directory fd binds, and reads the generated cache
with host ldconfig. A second run adds the GL tmpfs, a separately packed
extension bind and its library search path; the resulting cache must name
that extension's actual library. Both real-ldconfig cases passed on this host.
The packaged static musl bwrap also passed a host run with the fd-based `--args`
bundle, runtime/cache fd binds, GL tmpfs and extension bind, and stock
`ldconfig -X -C`; the generated cache contains the extension library path.
Run the full proxy suite in an external disposable copy next to `packs/` and
`gpu-forward-spike/`; its build step produces fresh loaders before testing.

The proxy, packs and top-level host suites pass. Device acceptance is pending
the coordinator's VulkanInfo installation on VM 5556 (still absent at 08:36
local time). No candidate APK has been deployed to that VM. The signed diagnostic
and candidate APKs have been built. No kernel, policy or image changes are
required.

Device checks, also applicable to Ryzen/RX 6600, Surface Pro 3 and ProDesk:

1. Install the signed subsystem update and an installed VulkanInfo stub with
   its declared runtime and GL packages.
2. Enable the stub's `cache/v3-trace` marker through root adb, force-stop that
   exact stub package, and launch it again. Keep the VM's current /data.
3. Collect `logcat -s MatonV3`. Verify the ldconfig bwrap mounts the runtime
   and extension, executes ldconfig, and exits zero; verify VulkanInfo then
   reaches Vulkan initialization. Record any later graphics error separately.
4. Relaunch once more to exercise the generated-cache reuse path. Check for
   SELinux denials and retain the complete trace for failures.

### Resident altstack replacement during VulkanInfo launch (2026-10-19)

The enforcing v39 VM's initial Flatpak exec exited 139 before Vulkan loading.
Stack-bound instrumentation showed SIGSYS running on a 32 KiB stack installed
by resident libc, replacing the proxy/adoption stack. The fault's RSP was below
that mapping. Increasing an arbitrary stack size would leave the replacement
bug intact. The x86 dual-ABI filter now routes resident `sigaltstack` calls,
including queries, through the existing virtual app-stack implementation;
only the raw proxy gateways can modify the kernel stack. Ordinary resident
signal actions remain native, with the existing SIGSYS-action guard.

The signals regression requests an app altstack through resident libc, checks
its virtual query result, and verifies that a SA_ONSTACK signal still executes
outside that app mapping. The full compat-proxy suite passes. Rebuilt/signed
APKs were installed with `adb install -r`, force-stopping the VulkanInfo stub
after each install. The next launch preserved the adoption stack (0x10d30 bytes)
and reached Vulkan device enumeration without the prior SIGSEGV. No SELinux,
kernel, image or AOSP changes were made.

## Venus extension merge-dir regression (2026-10-19)

The actual VulkanInfo stub on adb 5556 failed before ldconfig: creating
`GL/vulkan/icd.d/maton_icd.json` followed a packed `icd.d` symlink instead of
using bwrap's new GL tmpfs. The pack ownership check compared staging names
without normalizing `/newroot` and the detached oldroot view, and only
recognized mounts strictly below an index root. The VM also mounts a GL
index at exactly the path subsequently covered by tmpfs. Mounts now retain
whether their backing filesystem shadows an index, including across binds,
pivot aliases and private exec-state transfer (format 3). Canonical packed
bind sources retain their index. No executable permission is fabricated.

`tests/pack-host.py` now combines real OSTree indexes at `/usr`, GL and
GL/default, a runtime/extension symlink at `vulkan/icd.d`, tmpfs over GL,
and bwrap's merge-dir symlink creation followed by real ldconfig. The new
fixture fails on the pre-path-walk revision `360ba14^` as well as original
main; it passes with this fix. Thus the two recent path-walk commits are
not established as the introducing change. The older nested-mount check
from `ffbe609` did not cover staging or equal-root overmounts.

The trace's `unhandled 442 184` means x86_64 `mount_setattr` (442), decoded
as internal `PROXY_SC_mount_setattr` (184). Returning ENOSYS is intentional:
pinned bubblewrap bind-mount.c:mount_setattr_setup falls back to remounts,
and Flatpak's payload filter also returns ENOSYS for this syscall. It is
not the merge-dir failure and needs no new emulation for this launch.

Signed standalone builds, the full compat-proxy suite (including the extended
host pack test and both exec-state backends), and device-tree preflight pass. VM trace
`venus-out/evidence/launch4.log` confirms ldconfig finishes and bwrap reaches
`execvp vulkaninfo`. That exec is refused: the installed app's signed
`assets/matonos/objects.index` AND `original.index` describe all six payload
files (including bin/vulkaninfo) as mode 0100600. This is the installed
publication's metadata, not a SELinux exec denial. Both /app and the
fallback /usr lookup select that non-executable object. The loader correctly
returns EACCES. Do not work around this with an executable-bit override.
Repair/reimport the publication and update its signed splits without
uninstalling the stub; then force-stop and relaunch to continue GPU testing.
No RX 6600 listing is claimed yet. No image, SELinux or kernel change was
made. Keep VM 5556 running and keep the existing stub installed.

`--pack-backing TARGET DIRECTORY`, placed after the corresponding `--pack-mount`,
records that pack's deployment `files` directory explicitly. The launcher supplies
this for app, runtime and extension deployments before Flatpak starts. Lookups
through a deployment path use only its associated pack; bind/pivot aliases and
unrelated extension indexes do not determine ownership. Both names must be absolute;
the backing is normalized lexically. Identity mounts pass their target as backing.

### Resident graphics descriptors

Android EGL warmup opens driver descriptors before the Linux payload runs.
Those libraries and their cached fd numbers survive emulated exec, so Linux
close sweeps must preserve the descriptors as well. The loader snapshots the
fd set immediately before and after EGL initialization; only the newly opened
fds become resident-owned. Existing launch/control fds retain Linux semantics.
Native descriptor creation, duplication, SCM_RIGHTS reception and closure update
the ownership table. Descriptor lifetime syscalls are trapped even in the
resident BPF ranges; native mmap/ioctl and pthread/allocator operations keep
their existing routing. Linux close/close_range/exec sweeps skip resident-owned
fds, and dup2/dup3 cannot replace one. Native close releases ownership before
its number can be reused. This is runtime lifetime management, not confinement.

`tests/resident-fds.sh` exercises native open/dup/close/reuse and Linux
close, dup2, both forms of close_range, and CLOEXEC across an emulated exec.
VM verification (2026-10-21, Mesa 26.2.3 virgl): `tests/offscreen` in the
Extreme Tux Racer stub prints `center=255,0,0,255`, PASS, loader exit 0.
Before the fix, Mesa's cached render fd 72 had been closed and reused for a
proxy temp directory; `DRM_IOCTL_VIRTGPU_RESOURCE_CREATE` returned -1, producing
texture GL_OUT_OF_MEMORY and a pbuffer with no color read buffer. Android
malloc and page-size probes were healthy; no TLS executor change was needed.

### Resident pthread alternate-stack startup (2026-10-21)

The x86 filter lets resident bionic `sigaltstack` calls use the existing
resident-libc exemption. Bionic installs a new pthread's alternate signal
stack before unblocking signals; trapping there kills the thread with SIGSYS
before its adopted start routine can run. Linux payload `sigaltstack` calls
still reach the proxy's virtual stack implementation. `tests/resident-altstack.c`
installs the actual BPF and verifies a resident call succeeds with SIGSYS
blocked, while an otherwise identical payload call still traps. This fixes
Extreme Tux Racer's startup exit 159; it does not repair Xwayland presentation.

### Virtual NEWNET interface ioctls (2026-10-23)

Within virtual NEWNET, loopback interface requests are answered from the
virtual interface table regardless of the descriptor's socket family. This
covers `SIOCGIFINDEX`, `SIOCGIFNAME`, `SIOCGIFFLAGS`, `SIOCSIFFLAGS`, and
`SIOCGIFCONF`; the virtual loopback is interface 1 and starts up. The x86
payload filters match the interface ioctl request values for both x86_64 and
i386, then let the inherited proxy filter trap and dispatch ioctl. Avoiding a
second `SECCOMP_RET_TRAP` is required because stacked traps recursively enter
proxy signal handling. Other ioctl requests and behavior outside virtual
NEWNET remain unchanged.

`tests/net.c` covers AF_INET and AF_UNIX datagram control sockets, loopback
index/name/flags/configuration, and the host ioctl path outside virtual
NEWNET. The separate `TIOCGWINSZ` denial on a pipe is expected: Linux returns
`ENOTTY`, so it needs no virtual handling. No VM test was run.
