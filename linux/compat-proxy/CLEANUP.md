# Compat proxy cleanup inventory (phase 1)

Inventory captured for the `proxy-cleanup` branch on 2026-10-24. Counts are
source lines (`wc -l`), including comments and blank lines. Function spans are
approximate responsibilities: related helpers are intentionally grouped
because much of this code uses compact one-line C definitions.

## Core source inventory

| File | Lines | Function groups / purpose | Status |
|---|---:|---|---|
| `proxy.c` | 1,550 | Signal/crash reporting and fd guards (~1–150); i386 TLS/LDT emulation (~149–605); optional profiling and logging (~605–646); syscall policy/path translation/network/DNS/fd tracking (~647–995); SIGSYS routing, syscall dispatch and filter install (~996–1538) | Active central dispatcher. Profiling is opt-in; i386 TLS code compiles to stubs on x86_64. No removable unreachable handler found in this pass. |
| `loader.c` | 585 | Resident map collision checks and freestanding helpers (~1–146); ELF validation/map/stack and exec (~147–419); CLI, startup, state handoff and optional GPU bridge (~420–585) | Active on both ABIs. Host-only CLI flags support harnesses and are not production launch behavior. |
| `vfs.c` | 745 | allocator/arena and path cache (~1–145); temp ownership/GC (~146–210); mount/path resolution and symlink handling (~211–477); fd tracking, virtual mount operations and VFS state (~478–745) | Active core. Existing timing hooks are gated by `--count`; no separate v2 containment implementation remains. |
| `net.c` | 196 | virtual NEWNET fd aliases, route request validation/ACK queue, loopback ioctl results and state transfer | Active, intentionally narrow implementation. |
| `dns-unix.h` → `dns-netd.h` | 133 | DNS endpoint parsing; socket alias and port tracking; helper clone/epoll loop; netd request/reply framing; app socket rewrite and fork reset | Active behavior. The old app-facing Unix-socket forwarder is gone; this module's Unix socket is specifically the current netd `dnsproxyd` transport. Renamed to prevent confusing the two. |
| `state.c` | 112 | fixed-width stream helpers, state validation/import, sealed memfd creation, trusted cross-ABI exec handoff | Active v3 state protocol; despite the header comment's “spike” wording, this is required runtime state, not retired v2 containment code. |
| `pack-vfd.c` | 295 | packed immutable file descriptor operations and serialization | Active APK/deployment packaging path. |
| `links.c` | 312 | marked-store hardlink/index operations and recovery | Active OSTree deployment path. |
| `sysvipc.c` | 1,575 | shared IPC object/record model, sem/shm/msg operations, waits/wakes, fork/exec lifecycle and state | Active compatibility subsystem. Its `v2` shared-record version is a wire-format revision, not v2 security design. |
| `host-command.c` | 255 | resident host worker launch, environment/fd transfer and worker lifecycle | Active host integration. `HOST_COMMAND_TEST` branches are compiled only in dedicated host harnesses. |

## Architecture inventory

The architecture directory totals about 4,099 lines. Common contracts are in
`arch/arch.h`, `exec.h`, `ipc.h`, `socket.h`, `stat.h`, `statx.h`, `time.h`,
`termios.h`, and `syscalls.h` (internal operation IDs). `arch/x86-filter.h`
contains generated/multi-ABI filter construction.

| Area | Lines | Purpose and cleanup assessment |
|---|---:|---|
| `arch/x86_64/syscalls.h` | 779 | Canonical operation ↔ x86_64 wire syscall map. |
| `arch/i386/syscalls.h` | 755 | Same adapter shape with i386 numbers and legacy aliases (`mmap2`, `fstatat64`, `fcntl64`, etc.). The generator previously duplicated the encode/decode rendering loop; `gen-arch-syscalls.py` now renders both maps with one architecture-neutral routine. ABI tables remain distinct because syscall numbers/aliases differ. |
| `arch/{x86_64,i386}/backend.h` | 91 / 152 | Register context, argument/result conversion and gateway details; genuine ABI differences, not safe common-code candidates. |
| `arch/{x86_64,i386}/thread-adopt.c` | 582 / 444 | Thread, signal, TLS, robust-list and fork adoption. Shared responsibilities have different kernel TLS/context models; no safe type/constant-only extraction identified. |
| `arch/{x86_64,i386}/raw.S`, `entry.S`, `adopt.S` | 200 / 175 | Raw syscall/trap entry and adoption assembly. ABI register conventions make these architecture-specific. |
| `arch/{x86_64,i386}/adopt.h` | 25 / 65 | Public bridge/adoption ABI declarations; i386 has extra segment descriptors and versioned hooks. |

## Tests and support files

`tests/` contains 93 files (4,759 lines), split between host C/Python/shell
fixtures and i386 assembly/device checks. `tests/run.sh` is the main host
orchestrator; `tests/i386-p1.sh` is the required first-tier i386 run. The files
are currently referenced by the orchestrators or provide explicit device-only
coverage; no unreferenced test was deleted. Build scripts select architecture
sources and optionally enable `PROXY_RAW_COUNTER`. `gen-arch-syscalls.py`
reads the checked-out Bionic UAPI headers and writes the common IDs plus both
ABI maps.

## Removed or corrected in phase 1

- Renamed `dns-unix.h` to `dns-netd.h`: the old standalone app-facing Unix
  transport and forwarder were retired in the DNS rewrite. The remaining
  AF_UNIX connection is the active netd `dnsproxyd` protocol, so no runtime
  path was deleted and app-visible DNS behavior is unchanged.
- Reworked syscall map generation to use one renderer for x86_64 and i386.
  The generated syscall-number tables remain separate because wire numbers and
  legacy aliases differ. Generated headers are byte-identical after
  regeneration.
- Replaced the 1,574-line round log in `README.md` with a current design and
  operation guide plus a short dated changelog. Stale narrative no longer
  presents retired standalone DNS/forwarder architecture as current.
- No reachable v2 security/containment hook, unused production option, or dead
  test was safe to delete after cross-referencing current callers and tests.
  This is a deliberate inventory result, not a claim that all long functions
  are optimally structured.

## Phase 2 candidates backed by host measurements

The baseline `bash tests/run.sh` run on this host produced:

- `links.c`: 20,000 files / 5,000 links / 25,000 stats took 5.85 s plain and
  6.18 s through indexed storage (1.056×); 5,000 mutations took 5.08 s.
  The indexed path adds roughly 0.33 s in this workload. Profile marker lookup,
  placeholder conversion and stat/index reads separately before changing it.
- GPU sysfs fixture: 40,000 opens took 3.74 s and 4.09 s across the two
  variants (about 93–102 μs/open in this end-to-end test). Profile path
  translation, property fallback and actual raw syscalls separately; do not
  assume the VFS path cache is the bottleneck.
- No checked-in per-trap microbenchmark currently isolates handler overhead.
  `--count` already records completed handler/VFS nanoseconds, and
  `PROXY_RAW_COUNTER=1` records raw calls. Next add/use a repeatable payload
  loop to measure trapped `getpid`, `stat/open`, and a no-op/high-frequency
  syscall against native execution; report count plus elapsed ns and compare
  cached versus uncached VFS paths.
- The test suite's existing workload data points to link/index and filesystem
  metadata paths as the first measurement targets. Use profiles and counters
  to identify dominant functions before changing allocation, lookup or trap
  dispatch code.

## fd-bookkeeping rewrite baseline (2026-10-24)

Baseline on branch `proxy-fdbook`, before the fd-bookkeeping rewrite. The full
required build and suites passed. `tests/trap-bench.sh 20000 5` measured the
following medians (five fresh payload processes):

| Operation | Native | Proxied | Delta |
|---|---:|---:|---:|
| `getpid` | 317.6 ns/op | 409.0 ns/op | +91.4 ns/op |
| translated `open` | 2.28 µs/op | 109.46 µs/op | +107.18 µs/op |
| translated `stat` | 1.37 µs/op | 52.06 µs/op | +50.69 µs/op |

An instrumented 1,000-operation payload run (`PROXY_RAW_COUNTER=1`) reported
2,021 traps / 46,253 raw calls for open (22.88 calls/trap), and 1,021 traps /
15,253 raw calls for stat (14.94 calls/trap). The extra traps are the
loader/setup workload in the same process, so these ratios are an upper bound
for the isolated payload operation. Existing `--count` output attributes raw
calls by syscall ID, not by C call site; exact per-call-site counts require
adding a return-address counter before they can be claimed.

The active candidate sites along the translated open/stat paths are:

| Call-site group | Open path | Stat path | Reason / bookkeeping relation |
|---|---|---|---|
| Handler timing and signal routing (`proxy.c`) | clock reads around the trapped operation; signal mask operations only when chaining an app SIGSYS handler | same | Fixed handler work; not fd state. |
| Openat2 resolver (`vfs.c:resolve_openat2_fast`) | open backing root, `openat2` target, close backing root; on non-fd-return resolution read `/proc/self/fd/N` and close target | open backing root, `openat2` target, close backing root; fd-return variant avoids the proc readlink | Resolution work; the stat fast path then uses the retained fd. |
| Stat operation (`proxy.c` fast stat branch) | — | one `newfstatat(AT_EMPTY_PATH)` and close retained target fd | Metadata query and temporary resolver fd lifecycle. |
| Tracked open name (`vfs.c:vfs_fd_record`) | persistent path allocation, signal-mask + atomic lock, linked-list scan/update | — | Direct fd-bookkeeping cost; candidate for indexed in-memory table and pooled storage. |
| Other fixed trap-arena allocations (`vfs.c`) | allocation growth may `mmap`; arena end reuses blocks | same | Amortized workspace, not fd metadata. |

The syscall-ID counters also show why the aggregate cannot be read as a
call-site table: the process includes setup, timer and arena activity. In
particular, syscall IDs 213 (`openat`), 206 (`nanosleep`), 212
(`open_tree_attr`), 267 (`rt_sigpending`) and 27 (`clone3`) occur in the
instrumented run; only a subset belongs to the measured pathname handler.

## Continuation attempt 2: profiler-first attribution (2026-10-24)

`perf record -F 999 -g --call-graph dwarf ...` could not open the CPU cycles
event: this host has `perf_event_paranoid=4` and no `CAP_PERFMON`. I added an
opt-in `PROXY_PROFILE=1` build mode instead. It uses `lfence; rdtsc` around
selected handler/VFS sections and records `(internal syscall ID, return PC)`
raw-call counts and cycles. `-g -fno-inline` is used only in this diagnostic
build so `addr2line` resolves call sites. Normal builds have no profiling
overhead. The diagnostic `--count` path intentionally skips its own two
`clock_gettime` calls per trap; otherwise those calls dominate the profile.

The comparison used one-op and 20,000-op fresh payload processes against the
same mapped regular file; subtracting the one-op process setup yields the
following raw calls per measured operation. “Open” means the benchmark's
`openat` plus `close`, so it spans two trapped payload syscalls. Counts are
profile-site deltas, not inferred from a syscall-type aggregate.

| Operation | Internal syscall | Raw call site | Calls / operation | Effect |
|---|---|---|---:|---|
| open + close | `openat` | `links.c:57`, `tree()` → `marked_device()` ancestor marker probes | 20 | Repeated `.v3-store-root` lookups on both link-store checks. |
| open + close | `openat` | `proxy.c:1117`, `android_library_mapping_state()` | 6 | Handler setup / mapping checks. |
| open + close | `mmap` | `vfs.c:646-647`, `vfs_fd_record()` / `pdupstr()` | 2 | Persistent path and record allocation. |
| open + close | `munmap` | `vfs.c:413-417`, `fd_name_close()` | 2 | Frees old path and record. |
| open + close | `rt_sigprocmask` | `vfs_fd_record()` and `fd_name_close()` | 3 | Signal masking around fd-table updates. |
| open + close | `rt_sigprocmask` | handler and `dns_close()` | 3 | Signal routing / DNS lock bookkeeping. |
| open + close | `faccessat`, `newfstatat` | `proxy.c:791`, `path_call()`; `links.c:71`, `links_target()` | 2 | Fixed path and link-store checks. |
| open + close | `gettid`, `mmap` | handler / signal restore sites | 5 | Handler routing and signal-stack restore. |
| open + close | `getpid` | `dns_lock()` | 1 | DNS lock owner check. |
| **open + close total** |  |  | **44** | **22 raw calls per trapped syscall** (two payload traps). |
| stat | `openat` | `links.c:57`, `tree()` → marker probes | 10 | One link-store tree walk for this stat trap. |
| stat | `openat` | `proxy.c:1117`, `android_library_mapping_state()` | 3 | Handler setup / mapping checks. |
| stat | `openat` | `vfs.c:349`, `vfs_openat2_path_fd()` | 3 | Fast resolver descriptor setup. |
| stat | `newfstatat` | `proxy.c:1471`, handler result path | 2 | Metadata operations. |
| stat | `readlinkat` | `links.c:302`, `links_payload_fd_stat()` | 1 | Link-count repair lookup. |
| stat | `gettid`, `mmap` | handler / signal restore sites | 2 | Handler routing and signal-stack restore. |
| **stat total** |  |  | **21** | One trapped payload syscall. |

The assembly `proxy_restore` return PCs account for the reported `mmap`
bucket there; this is the signal gateway / restorer path rather than a pathname
allocation site. Section timers (TSC cycles, profile build; nested sections
overlap) recorded about 34k cycles in `vfs_fd_record()` per open, 2.3k cycles
per fd-name lock/unlock section, and handler totals of about 204k cycles/trap
for open and 186k cycles/trap for stat. Measured profiled latency was 115.3
µs/open and 55.8 µs/stat; compare with the uninstrumented baseline above.
These measurements make ancestor marker lookup and fd-record allocation the
first optimization targets.

## fd-table and immutable-index changes (2026-10-24)

`perf` remained unavailable (`perf_event_paranoid=4`, no `CAP_PERFMON`), so the
opt-in `PROXY_PROFILE=1` TSC/call-site build was used before and after the
changes. For each profile, the one-operation process was subtracted from a
20,000-operation process to remove loader/setup work. The final `--count`
aggregates corroborate the site deltas: open+close has 10 raw calls per pair
(5 per trapped payload syscall); stat has 5 raw calls per trapped syscall.

| Measured operation | Raw call site | Calls / operation | Result |
|---|---|---:|---|
| `openat` + `close` | `proxy_restore` (gateway signal-stack restore, `mmap`) | 3 | Runtime gateway work. |
| `openat` + `close` | `handler` (`gettid`) | 2 | Trap routing. |
| `openat` + `close` | `path_call` (variable raw `openat`) | 1 | Payload open. |
| `openat` + `close` | `fd_name_close` (`rt_sigprocmask`) | 1 | Protect fd-table update. |
| `openat` + `close` | `handler` (`rt_sigprocmask`) | 1 | Signal routing. |
| `openat` + `close` | `vfs_fd_record` (`rt_sigprocmask`) | 2 | Protect fd-table update. |
| **open+close total** |  | **10** | **5 raw calls per trapped payload syscall.** |
| `stat` | `handler` (`newfstatat`) | 2 | Original result and fast-path metadata. |
| `stat` | `vfs_openat2_path_fd` (`openat2`) | 1 | Resolve translated backing path; the temporary root-fd open/close pair is gone. |
| `stat` | `proxy_restore` (`mmap`), `handler` (`gettid`) | 2 | Gateway and trap routing. |
| **stat total** |  | **5** | **5 raw calls per trapped payload syscall.** |

The fd-name linked list is now a dynamically grown fd-indexed array. Close
clears a slot but retains its string capacity for reuse; there is no fd cap.
Fork/exec serialization still carries the prior fd/path wire data. Rename
updates tracked names, including descendants under a renamed directory.
Signal masks remain around table mutation because the table is shared by
threads and a signal handler that reentered a held lock could deadlock; locks
stay short and do not cover payload syscalls. `links.c` caches marked and
unmarked ancestor results with mutation-generation invalidation. Negative
Android-map detection is cached and invalidated on mapping changes. These
caches removed repeated marker probes and `/proc/self/maps` opens from the
measured path.

`objects.index` is precomputed with each immutable deployment and sorted by
path. The proxy keeps one parsed index with its mount for the deployment's
lifetime; binary search serves path/stat lookups and directory iteration seeks
to the sorted child range. Package updates create a different signed index and
mount, so no path/stat/listing cache is reused across deployment updates.

Final normal-build medians from `tests/trap-bench.sh 20000 5` (five fresh
processes, same filesystem):

| Operation | Native | Proxied | Ratio |
|---|---:|---:|---:|
| `getpid` | 321.89 ns/op | 426.35 ns/op | 1.32× |
| translated `openat` + `close` | 2.53 µs/op | 13.74 µs/op | 5.43× |
| translated `stat` | 1.40 µs/op | 7.58 µs/op | 5.40× |
| translated `statx` | 1.37 µs/op | 8.12 µs/op | 5.95× |

`tests/io-bench.sh 1073741824` compared 10,000 open/close, stat and statx calls
plus a sequential 1 GiB zero-filled allocated file on the same filesystem.
The final 10k metadata rates were 2.75/13.56 µs (open), 1.32/8.23 µs (stat),
and 1.50/7.62 µs (statx), native/proxied. The page-cache-warmed 1 GiB read
took 90.0/135.0 ms and mmap took 10.0/11.9 ms, native/proxied. These numbers
measure fd passthrough and memory bandwidth, not cold physical-disk throughput.
Read and mmap are not handled by the pathname index once the fd is open.
