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
fd duplication, NOREPLACE/EXCHANGE, EXDEV and EROFS.

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
