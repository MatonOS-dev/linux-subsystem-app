# Host I/O performance work, 2026-10-29

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
