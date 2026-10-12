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
usr/bin/lib/lib64/etc/sbin/proc/dev/data endpoints are resolved through the
host kernel and pinned as explicit mounts before payload symlink semantics
begin. This distinguishes bootstrap scaffold links from payload links.

Exec supports static and glibc ELF and shebangs. CLOEXEC descriptors close on
successful in-process exec. ELF program-header and argument/environment
vectors grow dynamically, with overflow checks. A 4 MiB minimum initial
stack grows for larger argv/env; PT_INTERP follows Linux's 4096-byte pathname
limit and script parsing uses Linux's 256-byte header size. Thread records
are dynamic; the previous 128-slot cap is gone.

**Bionic exec is not implemented.** PT_INTERP `/system/bin/linker64` is
explicitly rejected with ENOTSUP before mapping the main image. A real kernel
exec retains seccomp but resets caught signal actions and replaces proxy
memory. Thus it does not inherit the resident proxy merely by preserving a
mount table or environment variable. Bionic needs an entry/reentry design
that installs the handler before any trapped syscall, with Android linker
and TLS validation. Host bwrap tests use the installed glibc bwrap 0.9.0;
staged 0.13.0 sources were inspected, but its bionic binary was not executed.

SIGSYS remains deliverable across rt_sigaction/sigprocmask, sigreturn and
app altstack changes. The proxy preserves changes to other signal-mask bits
across sigreturn (required by bwrap's SIGCHLD signalfd monitor). Its private
1 MiB altstack is retained; chained app SA_RESETHAND and actual
SYS_USER_DISPATCH delivery are tested. App SA_ONSTACK semantics and multiple
user-dispatch owners remain unsupported. Thread records/private stacks are
not reclaimed after thread exit.

Remaining constraints: user pointers are not EFAULT-safe; mount/cache/cwd
mutations are not synchronized for concurrent pathname calls; reverse fd
mapping is ambiguous when multiple aliases share one backing path; mountinfo
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
It requires no Soong/Gradle/image build and performs no VM reboot or policy
change. VM tests were not run in this sandbox. Result report:
`/mnt/data/aosp/out/pc-logs/agents/compat-proxy-2-result.md`.
