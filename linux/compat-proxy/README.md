# Linux layer v3 — syscall compatibility proxy

Experimental x86_64 resident ELF loader and static library. Android's inherited
seccomp filter, UID and SELinux restrictions stay in force. No real namespace,
mount, ptrace, or Android change. The virtual filesystem is compatibility
state, **not containment**. Do not wire this into production stubs yet.

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

`tests/bionic-host.sh` copies the staged runtime/Flatpak APEX files into build/
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
user-dispatch owners remain unsupported. Thread records/private stacks are
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
org.matonos.linuxruntimes UID to run Stage 2 as root: the bionic Flatpak CLI
uses user-0 app UID 10115's DE install and the runtime app's DE install,
linuxd's global CLI HOME/XDG directories, and CE home for the bwrap shim.
TMPDIR/download tmp are deliberately confined to the test staging directory.
The bionic CLI gets no --root. stdout, pid-tagged trap/mount logs and status
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

`tests/bionic-host.sh` now also runs `tests/flatpak-bwrap-host.py`. Its
Flatpak 1.19-shaped fd bundles use --ro-bind-fd for /usr and /app,
--ro-bind-data for generated config, --bind-fd for the cache, user/PID/IPC/net
namespace setup, --disable-userns, proc/dev mounts, usr links and sysfs binds.
Sealed --args data drives the staged bionic shim and bwrap. The ldconfig -X
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

Round 5 inode publication: `linkat` preserves a final `/proc/self/fd/N`
magic link when following the source. `AT_EMPTY_PATH` is translated to that
proc form so publication does not require CAP_DAC_READ_SEARCH. Destination
resolution, virtual mount identity and read-only checks still apply. Other
proc-fd resolution (especially bind-mount sources) keeps its existing backing
path behavior. `O_TMPFILE` and `renameat2` flags pass to the kernel after
virtual path checks; `tests/publication.c` covers publication/replacement,
fd duplication, NOREPLACE/EXCHANGE, EXDEV and EROFS. Round 13 below
replaces the kernel O_TMPFILE/linkat replay described here.

Bwrap loads now log outer argv and NUL-separated `--args` bundles through
pread without consuming or changing the fd. `tests/flatpak-bwrap-host.py`
accepts `COMPAT_STAGE2_LOG` to replay the logged outer ldconfig command.
The fd bundle remains source-derived host test data: the round-4 log has no
expanded bundle and no app-launch invocation, so this does not reproduce
all VM deployments, metadata, extensions or permissions. Instance files,
locks, status/lifetime fds and actual seccomp installation are exercised by
the offline payload phase. No storage layout is hard-coded into the proxy.

Round-6 offline app-launch replay (after `tests/bionic-host.sh` prepares the
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

See ../compositor/V3-LAUNCH.md for the production spec, paths, retirement and
remaining image checks. Historical bionic replay fixtures reference old staged
APEX helpers and do not prove the new production path.

Round 26 DNS: `--dns-forwarder 127.x.y.z:53 --dns-socket /private/runtime/dns/forwarder`
configures an exact app-visible IPv4 resolver and its Unix transport base.
The stub listens on `<base>.udp` (SOCK_SEQPACKET) and `<base>.tcp`
(SOCK_STREAM), in an app-owned mode-0700 directory with mode-0600 sockets.
Both sides check the kernel's SO_PEERCRED UID against their own UID. There
are no UDP/TCP Internet listeners and no NETLINK_SOCK_DIAG calls.

On the first DNS connect/send, the proxy replaces the Internet socket's open
file description with the appropriate Unix socket while keeping all tracked
fd aliases, CLOEXEC, nonblocking flags, timeout/buffer options, and epoll
registrations. Kernel queues continue to supply poll/epoll readiness and
packet boundaries. A poll started before first DNS I/O rechecks unconverted
Internet fds in 10 ms slices, so it follows concurrent conversion. recvfrom/recvmsg and getpeername synthesize the original
IPv4 server at port 53. DNS TCP uses its ordinary two-byte framing and supports
persistent connections; truncated UDP responses trigger normal glibc TCP
fallback. Other Internet sockets stay native. The copied app filter remains
in force. NEWNET sockets cannot be converted, and a direct Unix connect to
the DNS path is also denied from NEWNET. V3Launcher does not start/configure a
forwarder without the stub's INTERNET permission. DNS remains a per-stub
public-API Java service using DnsResolver/netd as the app UID; linuxd/Binder
carry no DNS fds. UDP query tasks use bounded concurrency so one slow answer
does not stall other queries on the same channel.

`tests/run.sh` includes mocked-kernel production transport tests and attempts
`tests/dns-host.py` (explicit SKIP when this host forbids Unix socket bind).
Run `python3 tests/dns-host.py` directly to require real glibc UDP/TCP lookup,
automatic truncation fallback, parallel threads, timeouts, aliases, source
addresses, poll/epoll and sendmsg/sendmmsg. `tests/resolv-host.py` covers mapped
resolv.conf, getaddrinfo/getent, HostCommand and the no-network Flatpak case.
The Java DnsForwarderTest checks the exact UID predicate, not real kernel
credentials. Fresh enforcing-device testing must still confirm app-to-app
Unix socket access under stock SELinux and real netd responses.

Round 13 hard links on app-domain storage: `O_TMPFILE` now creates a unique
`.matonos-tmp-<pid>-<counter>` file in the requested directory with `O_EXCL`
and the requested mode (subject to the normal umask). It never asks the kernel
to hard-link an app file. The hidden inode has a shared reference/publication
record and a process-local fd table. `dup`, `dup2`/`dup3`, fcntl duplication,
fork/vfork/posix_spawn, resident exec and CLOEXEC closure preserve that record.
The last intercepted close, CLOEXEC closure or `exit_group` removes an
unpublished file. Fork reserves child references before cloning and rolls
back on failure. A small assembly clone retry preserves supplied child stacks;
ordinary thread clones retain the existing native path.

`linkat` following `/proc/self/fd/N` or `/proc/<pid>/fd/N`, and `AT_EMPTY_PATH`,
publishes a tracked temporary inode with `renameat2(RENAME_NOREPLACE)`.
Existing destinations return EEXIST and leave the hidden file available for
retry. The open fd still refers to the same inode, and proc fd names reflect
publication, including in an inheriting process. Re-publication through the fd
returns EPERM, so it cannot move an already published inode. `O_TMPFILE|O_EXCL`
remains unpublishable. Virtual mount EXDEV and read-only EROFS checks still
precede the backing operation. Another pid's proc fd must be accessible and
refer to a temporary inode tracked by the calling process (normally inherited
across fork); there is no general cross-process fd-registration protocol.

Round 14 replaces the named-file copy fallback with a hidden inode store.
On the first named link, the regular file moves atomically into
`<private-tree>/.v3-inodes/i-<128-bit-random-id>` and both names become
symlinks to that same backing file. Subsequent links add another name.
The store lives in the highest ancestor on the same filesystem owned by the
caller, writable/searchable by its owner, and without group/other write bits.
Both names must share such a private directory. Source ownership remains
required. Filesystem and virtual-mount boundaries return EXDEV; read-only
virtual mounts retain EROFS. Ordinary symlinks without AT_SYMLINK_FOLLOW and
nonregular sources retain EPERM. No app/host path is hard-coded.

Path resolution preserves store names instead of interpreting their backing
absolute symlinks in the virtual root. stat/lstat/statx and fstat report the
backing regular inode and persistent logical link count; readlink returns
EINVAL, O_NOFOLLOW opens the regular backing file, and metadata changes affect
that file. Directory enumeration hides `.v3-inodes` and reports its names as
DT_REG. Payload path resolution rejects the reserved store component.
Rename/unlink operate on the name; renaming one name over another name of the
same inode is a no-op. The final unlink removes the backing file and count
sidecar; already-open descriptors continue to work and report nlink zero.

Each store has a kernel flock lock, independent of proxy memory, so fork/exec
and independently launched proxies share the same serialization. Count
sidecars avoid a name-count ceiling and avoid traversing the tree for clean
metadata reads. A dirty marker precedes name mutations; interrupted mutations
recompute counts from names under the lock. A conversion journal records the
original name before its backing rename, allowing an interrupted conversion
to restore that name. Store entries lacking count metadata are checked for
orphans on the next path/listing access and removed if unreferenced. Recounts
traverse same-filesystem directories without following ordinary symlinks;
mutations currently recount all store objects (an optimization opportunity).
This protects against process death, rather than promising power-loss
transactions; the journal and directory updates are not fsync-ordered.

Store-root rename carries a relocation journal with the directory and repairs
absolute store targets under its existing lock. Repair is idempotent after
process death, and cwd identity is rechecked so relative paths follow a moved
working directory. Store-root RENAME_EXCHANGE is currently ENOTSUP; ordinary
file-name exchange continues through the kernel and count reconciliation.

Remaining limits: all same-filesystem subdirectories of a tree must remain readable during recount;
permission-denied recovery fails closed. Raw/unintercepted changes to names
are outside the protocol. The first conversion has a rename-to-symlink gap
for raw observers; proxied open/metadata/listing operations serialize against
it. Pre-existing native hard links are not imported as extra logical names.
These are compatibility limits; the proxy is not a security boundary.

Round-13 temporary-file limits remain: `.matonos-tmp-*` files are discoverable
and report physical nlink 1 before publication. Abrupt death, direct SYS_exit,
untracked fd transfers/manipulation, and moving a containing directory can
leave temporary files/shared reference records. Descriptor-table mutation and
fork continuation remain unsynchronized. CLONE_FILES without CLONE_VM returns
ENOTSUP; existing thread/CLONE_FS limits remain. Cleanup is best effort when
kernel permissions deny unlink.

`tests/run.sh` runs both publication fixtures under the copied app filter and
a preceding kernel filter returning EACCES for every linkat. The denial wrapper
first proves a raw linkat gets EACCES before launching the proxy. Tests cover
Flatpak-style proc-fd publication, AT_EMPTY_PATH, EEXIST/retry, rejection of a
second publication, duplicate-last-close cleanup, O_EXCL, child-first and
parent-first closure, resident exec/CLOEXEC, exit cleanup, failed clone rollback,
posix_spawn, shared named inodes with timestamps/mode/data, outside-tree and
symlink rejection, EXDEV, EROFS and renameat2 behavior. `tests/hardlinks.c`
adds shared-write, O_NOFOLLOW, statx/lstat/fstat/nlink, hidden-listing,
same-inode rename, last-unlink/open-fd lifetime, fork/exec concurrency and
independent loader-process tests, plus a store-root move and move-back.
Host-created interrupted transactions verify conversion restoration, orphan
collection, and interrupted store relocation on the next access. Host tests do not
reproduce SELinux. Combined round-13/14 host suite and the NDK r30/API-36 freestanding
loader build passed; no adb/device/image test was performed.

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
shutdown. See [HOST-COMMAND.md](../compositor/HOST-COMMAND.md) for protocol,
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

- The loader accepts ELFCLASS64/EM_X86_64 only, and its filter kills other
  syscall architectures. **This does not make Steam's 32-bit client work.**
  i386 needs an ELF32 loader, 32-bit signal/register and raw-syscall gateways,
  a copied i386 policy and IPC ABI translations. Those translations include
  ipc(117), semget/semctl(393/394), shmget/shmctl/shmat/shmdt(395–398),
  msgget/msgsnd/msgrcv/msgctl(399–402), and semtimedop_time64(420), as well as
  old multiplexer argument/structure/time layouts. Merely adding numbers
  to the present x86_64 dispatch is insufficient. x32 is also unsupported.
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
including its resolver bind and DNS translation state.

Stock Flatpak constructs resolver binds even with `--unshare-net`. At a
successful payload exec in a Flatpak root (`/.flatpak-info`), the proxy hides
`/etc/resolv.conf` and `/run/host/monitor/resolv.conf` for virtual NEWNET.
Construction is unaffected; the existing network filter continues to deny
connections. This state also reaches HostCommand children.

Validation: `tests/resolv-host.py --offline` reproduces the old missing map,
checks the canonical-map fallback decision and glibc resolver configuration
(127.x, port 53), and tests both direct-bind and monitor-symlink layouts with
and without NEWNET. Host/NDK builds and `tests/run.sh` pass. SDK-36 javac
compilation of V3Launcher with dependency fixtures passes. The full socket
variant `python3 tests/resolv-host.py` checks actual getaddrinfo queries at an
ephemeral forwarder port from both payload and HostCommand, but cannot run
in a sandbox forbidding host sockets. Device validation is pending: see
`out/pc-logs/agents/v3-resolv-result.md` for fresh-image steps. No kernel,
SELinux or shared-file changes are required.
