# I/O performance: round 2, 2026-10-29

Status: **partial; the overall <=2x-native requirement is not met**. Ordinary
sequential read/write now meet it in the paired host workload. Path operations
still miss it. Device fixed-cost measurement remains pending coordinator
capture; no device result is inferred from the host. No VM, image/Soong build,
push, framework patch or kernel change was performed.

## FD-specific sequential I/O

New signalfds, inotify instances and writable virtual
`/proc/sys/user/max_user_namespaces` memfds move into an I/O window. On the
usual high soft fd limit this is **[512, 1024)**; with a smaller initial limit
it is the upper half of the range below the private reserve and 1024. Staying
below 1024 keeps these descriptors usable by ordinary libc `fd_set`/select.
The existing private window is checked separately. Ordinary read/write/readv
outside these windows reach Linux without SIGSYS when tracing is off.

Aliases cannot be renumbered behind the caller's back: `dup2`/`dup3` return
the requested target and `dup`/`F_DUPFD[_CLOEXEC]` retain the kernel's
lowest-free/at-or-above rule. An emulated fd duplicated outside the window
gets a small **exact-fd seccomp routing filter**, installed with TSYNC before
it is returned. Fixed-target dup routes before replacing the destination.
The registry carries the filter ABI and survives private kernel exec handoff
(wire version 18); fork inherits filters and registry normally. New inotify
creators, `pidfd_getfd`, `recvmsg`/`recvmmsg` SCM_RIGHTS and initially inherited
special descriptors are covered. Source classification probes the live object
at adoption/dup time; a reused alias's ordinary file does not propagate traps
to its own duplicates. There is no pathname probe on an ordinary read.

**Allocation change:** initial signalfd/inotify creation and writable virtual
sysctl open deliberately return a window fd instead of the lowest free low
fd. These are the only creator return-number changes. In trace mode the old
allocation/routing behavior remains. A full window or lowered RLIMIT falls
back to the kernel's low allocation plus an exact-fd filter, rather than
inventing EMFILE while ordinary slots are free.

**Remaining routing/performance limitations:** classic seccomp filters cannot
be removed or relaxed. An exact alias number therefore remains trapped after
close/reuse, including an ordinary file later allocated there. Ordinary fds
inside the I/O/private routing windows also trap. These are explicit
exceptions to "every ordinary fd stays native" and are not claimed to meet
the 2x goal. Many distinct low aliases grow the kernel filter chain and its
cost; the kernel's filter-memory/instruction limits still apply. If TSYNC or
filter allocation fails, the error is returned (positive TSYNC failure TID
becomes EBUSY); fixed-target dup has not replaced its destination at that
point. Actual device TSYNC/SELinux behavior still needs validation.

## Emulation retained

- Signalfd read/readv consumer claiming and forwarded SIGCHLD metadata decode,
  including a record split across iovecs, duplicates, inherited fds and exec.
- Positive-length inotify EOF-to-EAGAIN workaround. Ordinary EOF remains zero.
- Successful writable sysctl-memfd writes synchronize the virtual namespace
  limit; duplicate tracking and lazy close/namespace synchronization remain.
- Readable namespace-map, overflow-ID, mountinfo and other synthesized memfds
  retain their contents and need no read-time translation.
- Existing private/resident descriptor lifetime behavior, fd flags and
  payload seccomp remain. The early gateway exemption now uses the actual
  architecture gateway address, including relocated host GPU fixtures.
- Path operations retain virtual mount/root translation, proc aliases, hidden
  resolver paths, read-only mounts, markers and indexed placeholder backing.
- Trace mode retains interception, zero-read diagnostics and stderr handling.

## Paired host measurements

Native/before/after are medians of five interleaved runs, 30,000 operations
per row, 128 file/directory pairs and positive/negative lookups. Baseline is
round-1 commit `2b0d733`. `openat` includes close in all variants. Access uses
faccessat2(F_OK), readlink uses readlinkat. This is a warm, EROFS-like directory
workload with virtual prefix mapping, not an actual mounted EROFS image.
Values are microseconds per operation:

| Operation | Native | Before | After | After/native | <=2x |
|---|---:|---:|---:|---:|---|
| read | 0.624 | 4.491 | 0.728 | 1.17x | yes |
| write | 0.768 | 4.710 | 0.987 | 1.29x | yes |
| openat | 2.209 | 13.180 | 12.869 | 5.83x | no |
| statx | 1.217 | 9.300 | 9.193 | 7.55x | no |
| access | 0.993 | 8.918 | 8.813 | 8.88x | no |
| readlink | 1.076 | 7.741 | 7.643 | 7.10x | no |
| openat missing | 1.280 | 11.001 | 10.965 | 8.57x | no |
| statx missing | 0.882 | 10.575 | 10.829 | 12.28x | no |
| access missing | 0.882 | 10.591 | 10.905 | 12.36x | no |
| readlink missing | 0.845 | 9.701 | 10.012 | 11.85x | no |

Path rows remain on SIGSYS plus VFS/link translation. The already measured
~3.5 us minimum proxy trap on this host exceeds the entire ~2 us budget for
native ~1 us metadata operations, even before path resolution. Caching alone
cannot achieve the target on that route. The profile also finds repeated
openat2 translation probes, final native file calls and fd metadata locking;
round 2 does not claim a path speedup. The mutable-tree negative-cache
correctness constraint from round 1 still applies.

## Fixed trap cost and profiling

`tests/io-trap-cost.c` is a static payload usable unchanged on the host and
through an installed stub's existing v3-command override. Without arguments it
runs three rounds of 30,000 loops and saves `$HOME/io-trap-cost.txt` ending in
`complete`. It separates native getpid/clock syscalls, a FICLONE handler that
answers without a payload-operation raw syscall, flock/fcntl handler+gateway,
zero-length I/O, sequential I/O and positive/negative path operations.
`tests/sigsys-cost.c` independently measures a tiny native SIGSYS handler and
return outside the proxy. `tests/trap-report.py` combines captures and OP
counter-inclusive times; it labels their limits explicitly.

An uncounted host diagnostic (three rounds, 10,000 operations) measured:

- Kernel-filter + minimal SIGSYS handler + return: **2.111 us** vs native
  getppid **0.270 us** (increment **1.841 us**).
- Proxy emulated ioctl: **3.526 us**; proxy flock **4.101 us** vs native
  flock **0.374 us**; proxy F_GETFL **3.938 us** vs native **0.264 us**.
- These are separate probes, not additive phase timings. Shell-native device
  baselines will also differ from the stub's UID/SELinux/seccomp context.

Host perf sampling remains unavailable. A separate PROXY_PROFILE build used
existing TSC/raw-call counters instead. Its latency numbers are not used as
headline performance (it uses -fno-inline and instrumentation). The profile
showed **one tail sigaltstack query per handled trap** (99,010 in that run),
36,005 openat2 resolution probes, 18,002 final openat calls and 9,002 fd-record
fstats. The profile-site key previously merged different operations sharing
a return address, mislabeling handler sigaltstack calls as an initial prlimit
probe; the key now includes the operation ID. The corrected single-threaded
capture records 99,012 sigaltstack SITE calls versus 99,010 RAWSC calls
(the profile starts before raw counters are enabled), and only 2 prlimit
SITE calls rather than conflating ~108,000 operations under that label.

**Device fixed cost: not measured yet.**
[DEVICE-IO-PERF.md](tests/DEVICE-IO-PERF.md) has exact coordinator commands to
compile, stage, launch through a disposable installed stub, capture uncounted
and counter-only runs, compare kernel signal cost, optionally compare tracing,
and restore the original app overrides. It changes app data only. Neither
these host results nor the old Steam aggregate distinguishes device signal
cost, SELinux/audit, logging and gateway cost without that capture.

Evidence: `/mnt/data/aosp/out/pc-logs/io-perf-round2/` (`paired-final.json`,
`trap-report.json`, `profile-fixed.err`, `profile-counts-fixed.txt`, suite logs).

## Validation

The production-filter regression checks ordinary read/write/readv as well as
positional/writev bypass, special-window routing, exact aliases, inherited
adoption and ordinary duplicates of a reused alias. The integration regression
covers flags, lowest-free dup, fixed targets in/out of the window, threaded
TSYNC, split signalfd readv, SCM_RIGHTS via recvmsg and recvmmsg, ordinary EOF,
full-window fallback, reduced RLIMIT and sysctl namespace-limit writes.
`bash linux/compat-proxy/tests/run.sh` passed with exit 0. Static benchmark
smoke runs and a separate PROXY_PROFILE build/capture also passed.
Existing suite coverage includes forwarded SIGCHLD, mixed ELF32/ELF64 exec,
resident ownership, GPU fixtures, bwrap, Flatpak and private state rejection.

# Round 1 historical results (superseded above)

Status: partial progress. The requested <=2x native target is **not met**.
Ordinary sequential read/write still trap. No VM, Soong/image build, push,
Android framework patch, or kernel configuration change was performed.

## Changes

- Route ordinary `pread64`, `pwrite64`, and `writev` directly to the kernel
  in the x86 routing filter. Keep the reserved fd window on its existing
  handler route. Trace mode retains interception and redirected-stderr output.
- Check cached link-store ancestry before issuing placeholder metadata probes.
  Outside a marked store, ordinary opens, stats and access checks require no
  placeholder lstat. Store recovery, indexed placeholder substitution and
  synthetic link counts retain their existing paths.
- Reuse the existing translation cache for stat and no-follow lookups, keyed
  separately by follow mode. Stat no longer unconditionally opens O_PATH,
  stats that descriptor, and closes it before considering the cache.
- When openat2 with NO_SYMLINKS returns ENOENT/ENOTDIR, use the translated
  path for the final operation without walking every component again.
  **Do not cache this negative translation or its errno.** Another process
  can create an absolute symlink into a virtual mount after the miss; a stale
  translation would let Linux follow that link outside the virtual namespace.
  The new cross-process regression covers exactly this case. Kernel negative
  dentries are still reused. Immutable-image negative metadata caching remains
  outstanding; no unsafe mutable-tree negative cache was added.

## Emulation cases retained

- `read`/`readv`: claim the current signalfd consumer and decode forwarded
  SIGCHLD envelopes into the original child notification. This is needed when
  resident threads from a previous exec receive the kernel notification.
  `readv` can split a record over multiple buffers.
- `read`/`readv`: positive-length inotify EOF becomes EAGAIN; ordinary EOF
  remains zero. Trace mode reports zero reads and descriptor targets.
- `write`: synchronize the synthesized max_user_namespaces sysctl memfd into
  virtual state after successful writes. Existing close/dup/close_range
  synchronization and duplicate tracking remain.
- Private/reserved fd routing and resident descriptor lifetime/exec ownership
  remain. Resident fds are not the reason every read traps: the resident call
  predicate covers lifetime operations, not read/write.
- Namespace-map, overflow-ID, mountinfo and other synthesized readable memfds
  keep their existing native file contents. They need no read-time data
  translation. There is no pack read-time copy/emulation in these handlers.
- Path operations retain virtual mount/root translation, proc aliases,
  hidden resolver paths, read-only mount enforcement, marker recovery and
  link-store placeholder translation. Existing payload seccomp behavior and
  both x86 ABIs remain covered by the suite.

A static classic-BPF fd window alone cannot preserve all current read/write
cases: dup2/dup3 can put a signalfd, inotify fd, or writable sysctl memfd at an
arbitrary ordinary descriptor number. The filter cannot consult the changing
userspace ownership table or relax a previously installed filter. Moving only
initial allocations to a high window would silently break these aliases.
Completing native read/write requires changing how these emulations handle
arbitrary aliases, not simply deleting read/write from ours(). This work does
not claim that architectural change is complete.

## Reproduction

Build with `bash linux/compat-proxy/build.sh`, then run:

```
python3 linux/compat-proxy/tests/io-perf.py --before /path/to/baseline-proxy
bash linux/compat-proxy/tests/run.sh
```

Baseline revision: `97ada32` on io-perf. Baseline proxy was compiled with the
same host flags as build.sh from the original proxy.c, vfs.c, links.c and
arch/x86-filter.h; the other source files were unchanged.

The benchmark compiles its C payload into an automatically removed directory
under /mnt/data/aosp/out/pc-logs. It uses 128 directory/file pairs and a symlink,
with positive and negative lookups, and maps that backing tree to /io-tree.
It is a warm-cache, EROFS-like directory workload, **not an actual mounted
EROFS image**. Each variant performs 30,000 operations per row; reported
values are medians of five interleaved native/before/after runs. Open includes
close, consistently in both native and proxied measurements. Access uses
faccessat2(F_OK); readlink uses readlinkat. Read/write use actual sequential
read/write syscalls, with enough file data to avoid EOF or wraparound.
The generic syscall entry deliberately measures the routing filter rather
than assuming that every application's callsite can be rewritten.

Timing results and complete logs are under
`/mnt/data/aosp/out/pc-logs/io-perf/`.

`perf record` was attempted but refused by perf_event_paranoid=4. A subsequent
passwordless sudo attempt was refused because a password is required. No
host sysctl was changed. Thus no perf sampling profile is claimed. Separate
strace -c captures supply syscall-count evidence; strace timings are not used
as operation latency measurements.

## Measured results

Microseconds per operation; all rows miss the <=2x requirement.

| Operation | Native | Before | After | After/native |
|---|---:|---:|---:|---:|
| read | 0.625 | 4.264 | 4.327 | 6.92x |
| write | 0.759 | 4.615 | 4.630 | 6.10x |
| openat | 2.189 | 17.430 | 12.614 | 5.76x |
| statx | 1.215 | 10.492 | 8.952 | 7.37x |
| access | 0.996 | 13.129 | 8.456 | 8.49x |
| readlink | 1.088 | 12.605 | 7.677 | 7.06x |
| openat-miss | 1.270 | 12.471 | 10.645 | 8.38x |
| statx-miss | 0.873 | 16.367 | 10.312 | 11.81x |
| access-miss | 0.872 | 11.806 | 10.106 | 11.59x |
| readlink-miss | 0.842 | 16.338 | 9.426 | 11.19x |

Read/write are effectively unchanged; the small differences are run-to-run
noise, not a claimed optimization. Positive path operations improve by
15–39%; negative lookups improve by 14–42% in this workload.

For 1,000 operations per benchmark row (including startup), strace recorded
92,883 total syscalls before and 65,803 after. Redundant newfstatat calls fell
from 23,857 to 26; openat2 calls fell from 4,263 to 4,136. The latter deliberately
retains fresh negative lookups for cross-process symlink correctness. These
are syscall-count observations, not timings derived from strace.

## Validation

- Full compat-proxy tests/run.sh, including host, i386, Android NDK builds,
  resident fd/exec, signalfd/child notification, hardlinks, EROFS, seccomp,
  and rewrite regressions.
- io-routing.c installs the production app and proxy filters and counts real
  SIGSYS deliveries: none for ordinary positional I/O/writev; private-window
  accesses retain interception; invalid negative fds stay native EBADF.
- io-cache.c checks a miss followed by creation in a separate forked process,
  including an absolute symlink whose target crosses the virtual mapping.
  Run both mapped and unmapped via io-cache.py.
- No device latency, Steam bootstrap time, or real-PC speedup is inferred
  from these host results. Device validation remains outstanding.

## Remaining requirements

All six requested operations still exceed 2x native in this host benchmark.
Read/write retain SIGSYS delivery and handler overhead for the cases above;
path operations retain SIGSYS, translation/cache bookkeeping, and a final
kernel lookup. The changes reduce redundant probes but do not remove these
fixed costs. Immutable image metadata/negative caching, fully native ordinary
read/write, permitted perf sampling, and the 2x target remain unfinished.
There are no shared device-tree or kernel changes requested by this commit.
