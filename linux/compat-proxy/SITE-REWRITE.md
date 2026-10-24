# Syscall-site rewriting and proxy measurements

Version 1 implements opt-in **load-time** x86_64 rewriting with
`MATON_PROXY_REWRITE=1`. The default remains SIGSYS. Only complete, unambiguous
ELF function symbols that the conservative backend can decode qualify.
Anonymous/JIT code is never scanned. Supported architectures going forward are
x86_64, i386 and arm64; the latter two retain SIGSYS in this phase.

### Trapped no-op baseline (2026-10-24)

`tests/trap-bench.sh 20000 5` compares native execution with an unknown x86_64
syscall (`1000`) that the proxy traps and answers with `-ENOSYS` without a
kernel syscall. This isolates signal delivery, proxy dispatch and sigreturn;
the proxy's one-time unknown-syscall diagnostic is redirected away from the
timed output. Host results, median of five fresh-process runs:

| Mode | Median | Range |
|---|---:|---:|
| Native unknown syscall | 314 ns/op | 280–328 ns/op |
| Trapped no-op (`-ENOSYS`) | 4.82 µs/op | 4.77–5.35 µs/op |

This is a host microbenchmark, not an Android device result. The proxied
unknown syscall is intentionally unsupported and is not a rewrite candidate.

### Metadata I/O baseline (2026-10-24)

`tests/io-bench.sh` ran once on the same host (10,000 metadata operations):

| Operation | Native | Trapped proxy |
|---|---:|---:|
| openat + close | 2.63 µs/op | 14.02 µs/op |
| newfstatat | 1.42 µs/op | 7.86 µs/op |
| statx | 1.36 µs/op | 7.93 µs/op |

These are single-run baselines; the script's sequential 1 GiB read and mmap
checks also passed, but do not provide a repeated timing distribution. These measurements precede the implementation; the repeated comparison below
uses the same fixed-number wrappers for all three routes.

## Phase 1 results (2026-10-24)

Host: x86_64, glibc 2.39, kernel 7.0.0-38-generic, GCC 13.3.0. No VM.
`tests/trap-bench.sh 20000 5`: median of five fresh-process runs, counts off.
Native, trap and rewrite all execute the same fixed-number wrappers.

| Operation | Native µs/op | SIGSYS µs/op | Rewrite µs/op | Reduction vs SIGSYS |
|---|---:|---:|---:|---:|
| Unknown syscall/no-op | 0.291 | 4.831 | 4.747 | 1.7% |
| getpid (native bypass) | 0.306 | 0.417 | 0.427 | -2.5% |
| openat + close | 2.482 | 13.340 | 8.881 | 33.4% |
| newfstatat | 1.356 | 7.694 | 6.009 | 21.9% |
| statx | 1.396 | 7.624 | 6.196 | 18.7% |

The no-op and getpid are controls: neither is rewritten; their small changes
are run variation. The file-class reductions are 33.4%, 21.9% and 18.7% for
these primary-encoding wrappers, not estimates for a whole application.
The final implementation also copies the six dispatch arguments before VFS
translation so the assembly save area always restores the original registers.

`tests/io-bench.sh` (10,000 metadata operations, one run per route):

| I/O metadata operation | Native µs/op | SIGSYS µs/op | Rewrite µs/op |
|---|---:|---:|---:|
| openat + close | 2.393 | 13.497 | 9.268 |
| newfstatat | 1.515 | 7.470 | 6.384 |
| statx | 1.850 | 7.709 | 6.097 |

The script's 1 GiB sequential read and mmap checks passed for every route.
Those syscalls are outside the rewrite class; their timings are not evidence
of rewriting large I/O. The temporary 1 GiB fixture was removed by the script.

One counts-off `tests/site-coverage.c` trace measured these scanner costs,
including ELF reads, proofs and page publication (not complete dynamic-loader
time): libc's 1,609,728-byte executable mapping **0.589 ms**, with 10 unique
sites patched; libstdc++'s 1,343,488-byte executable mapping **0.522 ms**, with
zero eligible sites. Its libm/libgcc dependencies took 0.088/0.036 ms. These
are single-run measurements, not timing distributions. The fixture library
patched 11 sites in 0.160 ms initially; two true unload/reloads hit the plan
cache and republished them in 0.132/0.152 ms. The cache avoids decoding; it
still validates and copies pages. libstdc++ remained resident after dlclose,
so its apparent reload is not presented as a cache benchmark.

| Host glibc 2.39 wrapper | Patched primary sites | Remaining primary sites |
|---|---:|---:|
| openat | 0 | 2 |
| close | 0 | 2 |
| fstatat | 1 | 0 |
| __fxstatat | 1 | 0 |
| statx | 1 | 0 |
| fstat | 1 | 0 |
| readlink | 1 | 0 |
| readlinkat | 1 | 0 |
| getdents64 | 0 | 1 |
| lseek | 1 | 0 |
| fcntl | 0 | 2 |

These selected symbols are not a whole-libc denominator. Stack-framed
openat/close/fcntl are deliberately skipped; getdents64 needs a currently
unsupported conditional-move opcode. Supporting these real glibc wrappers is
the first coverage follow-up. The simple wrappers in the integration fixture
prove every listed file syscall class end to end, including fcntl slow fallback.
Register/flags/SSE preservation and readlink's argument-modernization path have
explicit assembly checks. Overlapping symbols, execute-only file maps and
anonymous mremap replacements also have trap-fallback tests.

## Host measurements

Host: Flatpak 1.14.6, x86_64, Flathub ref
`app/org.freedesktop.Platform.VulkanInfo/x86_64/26.08` (commit
`12a5a61c6fbab4f4f001e7d27e6cf63d7f90ccb2698e02a3ad0fe00888620435`), with
runtime `org.freedesktop.Platform/x86_64/26.08`. The isolated test user install
used a temporary `XDG_DATA_HOME`. Pull and deploy are Flatpak's separate
`--no-deploy` and `--no-pull` operations. Native cold pull transferred the
runtime and app into a 663 MiB OSTree repo; the proxy comparison used warm
objects in that same repo, avoiding a second 663 MiB network transfer.
Launch measurements ran headless with `DISPLAY` unset.

### Per-trap allocation follow-up (2026-10-19)

The SIGSYS handler now uses a growable per-thread arena carved from its
alternate signal stack. `vfs_alloc()` bump-allocates zero-filled temporary
objects from that arena and reuses its blocks at handler exit; it maps another
block only when needed. The allocator does not call libc or take locks. Objects
that outlive a trap use `vfs_palloc()` explicitly, including mount/fd state,
pack indexes, DNS socket tracking and other linked records. This separation
was necessary for forked and duplicated DNS sockets: keeping those records in
the trap arena made them disappear when the handler returned.

The syscall counter is enabled only with `PROXY_RAW_COUNTER=1 ./build.sh` in
proxy builds used for host profiling; normal host, i386 and Android builds
omit it, so the hot path has no counter call or branch.
The `--count` format records RAW calls per completed trap and is reset in fork
children so parent history is not intentionally re-added. EUID reads for the
link-store ownership checks are cached atomically; marker, path existence and
symlink contents are still checked on each operation. Mount translation walks
the current mount table directly (mounts are added and removed during bwrap
setup), and pack indexes are already in-memory lookups; neither operation
issues a kernel metadata probe by itself.

For a same-host before/after comparison, I built `origin/main` and this branch
and traced the same warm, headless VulkanInfo Flatpak launch with
`strace -c -f`. These counts include the whole process tree and are syscall
entries observed by strace, not proxy-only counts:

| Kernel syscall | Before allocator | After allocator |
|---|---:|---:|
| `mmap` | 325,722 | 4,972 |
| `munmap` | 323,666 | 2,831 |
| `openat` | 413,426 | 413,175 |
| `readlinkat` | 44,035 | 44,006 |
| `newfstatat` | 5,987 | 5,984 |
| `geteuid` | 24 | 24 |
| All syscalls | 2,331,738 | 1,731,364 |

The allocator removed 641,585 observed `mmap`/`munmap` calls in this launch;
the total observed syscall count fell 25.8%. Warm launch wall time was 1.71 s
with counters off and 1.76 s with counters on. Strace itself took 55.2 s before
and 27.2 s after, so traced duration is not a wall-time comparison. The
`--count` file may contain repeated cumulative summaries when several threads
in one PID exit. For process-tree totals, take the final `RAW` and `RAWCAT`
record per PID, then sum unique PIDs. In the paired traced run this gave
13,814 trapped calls and 635,208 RAW calls (45.98/trap). The RAW syscall
categories were `openat` 411,048, `readlinkat` 43,954, `newfstatat` 4,705,
`geteuid` 4, `mmap` 3,655 and `munmap` 2,390. Strace saw 413,136, 44,001,
5,983, 24, 4,969 and 2,833 respectively across the full process tree; the
small differences are calls outside counted proxy handler frames. This is a
validated host after-rate. The coordinator's earlier VM install-loader
baseline was 2.53 million calls / 6,451 traps = 392.2 calls/trap; that is a
different workload, so the rates are not a direct A/B comparison. The paired
host strace totals above are the allocator before/after comparison.

The same isolated installation's combined warm pull-plus-deploy took 2.23 s
native, 4.83 s through the proxy with counters off and 4.91 s with counters on.
The counted proxy run reported 60,277 trapped calls and 1,856,714 RAW calls
(30.80/trap), taking the final cumulative summary per PID as above. The
counters added 0.08 s (1.7%) to this operation.

| Operation | Native before | Proxy before, counters off/on | Native after | Proxy after, counters off/on |
|---|---:|---:|---:|---:|
| Cold pull (`--no-deploy`) | 12.09 s | not run cold | not rerun cold | not run cold |
| Warm pull/reinstall (`--no-deploy`) | 1.31 s | 14.57 / 16.91 s | 2.30 s | 8.53 / 8.03 s |
| Warm deploy/reinstall (`--no-pull`) | 0.85 s | 12.37 / 15.48 s | 0.92 s | 1.81 / 1.75 s |
| Warm pull + deploy | 2.16 s | 26.94 / 32.39 s | 3.22 s | 10.34 / 9.78 s |

The proxy warm pull-plus-deploy wall time fell 61.6% with counters off. The
before-change counter run had 6,330,940 traps (6,292,634 `fcntl`) and 71.39 s
of aggregate handler time; after the filter change it had 81,531 traps and
14.03 s of handler time. Counter-on and counter-off wall timings varied by
about half a second in both directions at this workload size. Native timing
varied from 2.16 to 3.22 s between measurement sessions. Native cold pull plus
deploy was 13.51 s. A cold proxy network pull was deliberately not repeated.
The full MatonOS installer path could not run on this host: its Android-side
PackageInstaller/bridge transaction is absent. The host measurement covers
Flatpak pull and deployment, then the pack tool separately.

`linux/packs/tests/run.sh` completed in 0.90 s. It creates three small real
bare-user OSTree commits and exercises pack creation, update/reuse policy, and
checkout reconstruction. This is a synthetic pack microbenchmark, not a
production app's APK/carrier packaging time.

### Trapped-call profile

The first two tables are the before-change top-20 profiles for the warm proxy
pull and deploy with `--count`. Time is cumulative across processes and
threads; calls can overlap. `read` time includes time blocked in the kernel,
so these are handler elapsed times, not CPU samples. The after-change profile
had zero trapped `fcntl` calls; the safe commands bypass the proxy filter.

| Syscall | Count | Handler time (s) |
|---|---:|---:|
| fcntl | 6,292,634 | 7.987 |
| fstat | 9,301 | 0.050 |
| read | 7,147 | 58.688 |
| openat | 5,883 | 3.257 |
| close | 5,828 | 0.066 |
| lseek | 3,090 | 0.005 |
| getdents64 | 1,778 | 0.046 |
| mmap | 1,760 | 0.010 |
| statx | 874 | 0.399 |
| poll | 466 | 0.321 |
| write | 381 | 0.001 |
| newfstatat | 243 | 0.115 |
| readlink | 239 | 0.080 |
| recvfrom | 228 | 0.001 |
| unlinkat | 135 | 0.077 |
| recvmsg | 82 | <0.001 |
| mkdirat | 80 | 0.054 |
| execve | 78 | 0.113 |
| rt_sigprocmask | 72 | <0.001 |
| rt_sigaction | 60 | <0.001 |

| Syscall, sorted by handler time | Count | Handler time (s) |
|---|---:|---:|
| read | 7,147 | 58.688 |
| fcntl | 6,292,634 | 7.987 |
| openat | 5,883 | 3.257 |
| statx | 874 | 0.399 |
| poll | 466 | 0.321 |
| newfstatat | 243 | 0.115 |
| execve | 78 | 0.113 |
| readlink | 239 | 0.080 |
| unlinkat | 135 | 0.077 |
| close | 5,828 | 0.066 |
| mkdirat | 80 | 0.054 |
| fstat | 9,301 | 0.050 |
| getdents64 | 1,778 | 0.046 |
| linkat | 28 | 0.039 |
| renameat | 16 | 0.022 |
| rename | 12 | 0.016 |
| clone | 18 | 0.012 |
| mmap | 1,760 | 0.010 |
| symlink | 8 | 0.005 |
| unlink | 8 | 0.005 |

There were 6,330,940 trapped calls before the change. Summed handler elapsed time was 71.39 s,
including 3.62 s in VFS resolution. That is 11.27 us of handler elapsed time
per trap, with 0.57 us/trap in VFS resolution and 10.70 us/trap in the rest of
the handler. The aggregate handler time is 220% of the 32.39 s counted wall
time because multiple threads/processes were in handlers at once; this is
thread-time, not a wall-time fraction. Much of the high `read` time is blocking
I/O, which site rewriting cannot remove.

For launch, native `flatpak run --command=vulkaninfo` took 0.33 s before and
after. Proxy launch fell from 4.70 s off / 5.66 s on to 1.20 s off / 1.24 s
on. VulkanInfo exited with `ERROR_INCOMPATIBLE_DRIVER`: the host has no Vulkan
driver. Before the change, launch saw 1,054,631 traps, including 1,048,676
`fcntl`; afterward it saw 12,018 traps and no trapped `fcntl`. After-change
handler elapsed time was 2.074 s, of which 0.128 s was VFS resolution. As
before, aggregate handler/wall ratios can exceed 100% because threads run
concurrently; they are not serial time fractions.

| Workload | Traps before | Traps after | `fcntl` before/after | Proxy wall before/after (off) |
|---|---:|---:|---:|---:|
| Warm pull + deploy | 6,330,940 | 81,531 | 6,292,634 / 0 | 26.94 / 10.34 s |
| VulkanInfo launch | 1,054,631 | 12,018 | 1,048,676 / 0 | 4.70 / 1.20 s |

The smaller proxy bwrap fixture (`bwrap --ro-bind / / --tmpfs /tmp --proc
/proc --dev /dev /bin/true`) completed in 0.04 s with 325 trapped calls,
27.6 ms in handlers and 21.4 ms in VFS resolution. Its serial handler share
was about 69% of wall time: 84.9 us/trap total, 65.8 us/trap in resolution,
and 19.1 us/trap in the rest of the handler. It identifies the launch/setup
case where path resolution dominates, unlike the Flatpak install profile's
high-volume descriptor and blocking-read traffic.

### Improvement estimates, ranked

The first item is now measured; remaining items are estimates.

1. **Allow `F_GETFD` and `F_SETFD` through the filter.** Implemented and
   measured: trapped calls fell 98.7% on warm pull/deploy and 98.9% on launch;
   warm proxy pull/deploy fell from 26.94 s to 10.34 s and launch from 4.70 s
   to 1.20 s with counters off. Other `fcntl` commands remain trapped.
2. **Per-trap allocation and probing.** Implemented (9246db2): a per-thread
   trap arena replaces one `mmap`/`munmap` per allocation; EUID is cached.
   Host launch `mmap`+`munmap` fell from 649,388 to 7,803; raw kernel calls per
   trap are ~46 (launch) and ~31 (warm pull+deploy); warm pull+deploy through
   the proxy 4.83 s (native 3.2 s). The remaining per-trap `openat`/`readlinkat`
   probing is the next target.
3. **Syscall-site rewrite.** The measured residual handler cost is about
   10.7 us/trap for the install profile and 5.9 us/trap for the VulkanInfo
   launch profile. Multiplying by call count gives 67.8 aggregate handler
   seconds for the install and 6.2 s for launch; concurrency and blocking
   reads mean these are not wall savings. A conservative first target is
   roughly 2–6 s per warm install and 0.5–1.5 s per launch, to be verified by
   an A/B build. The serial bwrap fixture has a much larger per-trap cost but
   only 325 traps, so its direct ceiling is 27.6 ms.
4. **Resolution cache and app-data fast path.** The measured resolver total
   is 3.62 aggregate seconds for install and 0.40 s for launch; repeated
   metadata and path operations (`openat`, `statx`, `newfstatat`, `readlink`)
   dominate it. A cache hit for immutable mount-table translations and a
   safe direct path for verified app-data roots could save a fraction of this
   (estimate 0.5–1.5 aggregate seconds/install, 0.1–0.3 s/launch). Cache only
   translations; never cache existence or symlink contents.
5. **Install-process filter without path traps.** Flatpak's install/deploy
   process uses many paths, so disabling all path traps is not safe with the
   current virtual mount semantics. A narrowly identified host-only install
   process could bypass translation only for paths proven outside virtual
   mounts. Potential saving is bounded by path-handling time (3.62 aggregate
   seconds here); likely under 1 s wall for this sample. Do not exempt the
   bwrap sandbox payload.

## Version 1 implementation (2026-10-24)

### Load-time proof and publication

`rewrite.h` is the shared ELF/mapping core, selected through
`ARCH_REWRITE_HEADER`; `arch/x86_64/rewrite.h` owns instruction decoding, site
encoding and entry generation. There are no architecture-name conditionals in
this core. Other backends supply the disabled fallback. ELF records and offsets
remain fixed-width; addresses pass through `uintptr_t`.

The loader invokes the scanner on the main executable and interpreter's private
file-backed executable mappings before entering either image. The same mapping
helper handles libraries/dlopen before returning from trapped mmap. Initially
readable, private file mappings are recorded for a subsequent trapped mprotect
that adds execute permission. If the descriptor has closed/reused, tracking is
exhausted, the mapping overlaps an unsupported operation, or any proof fails,
leave the code trapped. Noncongruent ELF segments and anonymous mappings are
not rewrite candidates.

The scanner reads both `.dynsym` and `.symtab`, accepts `STT_FUNC` ranges of
1–4096 bytes, and linearly decodes the **entire** function before modifying any
site. Differently bounded overlapping functions, unknown instructions, stack
references/stack mutation, indirect control flow and incomplete symbols reject
the function. The first decoder intentionally proves a strict subset of leaf
wrappers; it does not yet decode FDE-only functions or perform stack/control-flow
data-flow analysis. No raw byte-match scanner authorizes a patch.

Every in-function direct branch must land on a decoded instruction boundary.
No branch may land inside a replacement window except at its first byte.
Page-straddling windows remain trapped. A function can contain several sites;
all function proofs precede its first page replacement.

For each accepted window, copy the original page into a separate anonymous RW
mapping, patch that copy, change it to RX, then publish with fixed `mremap`.
The original file mapping is never writable. A failed allocation/protection/
publication leaves the original window trapped. Every byte outside the window
is preserved, and normal original-VMA munmap/dlclose ranges remove the copy.
The replaced page loses its file identity in `/proc/self/maps`; rewriting is
an explicit opt-in for this reason. There is no first-SIGSYS discovery,
thread parking, JIT rewriting or modification of already-running code.

Scan plans are cached by device/inode, size, nanosecond mtime/ctime, packed
carrier slice, file offset and mapping length. Cached windows retain the
original bytes and are revalidated before publication. The process cache has
64 mapping plans and 512 windows/plan; saturation falls back to rescanning or
leaves extra sites trapped. Cache contention never waits in a signal frame;
that mapping remains trapped. No persistent text-address registry is required
for published pages.

### One entry table, no per-site stubs

One 16 KiB anonymous table is allocated below 2 GiB with `MAP_32BIT`, populated
RW then made RX. Its 512 entries have stride 32:

```
entry(nr) = table_base + nr * 32

before: b8 <nr:32>       0f 05        mov $nr,%eax; syscall
 after: b8 <entry:32>    ff d0        mov $entry,%eax; call *%rax
```

The 9-byte `48 c7 c0 <nr:32>; 0f 05` form uses the same 7-byte call plus two
NOPs. Each table entry begins with ENDBR64 and jumps to the shared assembly
entry. `%rax` identifies the syscall from its entry address; the pushed return
address is the original continuation. There is no per-site code allocation.

The entry saves flags, all syscall-preserved argument registers and the scratch
callee-saved register it uses, aligns the C stack and saves/restores legacy
FP/SSE state with FXSAVE64/FXRSTOR64. C code uses the ordinary ABI, preserving
the remaining callee-saved registers; only rax/rcx/r11 may change at return.
The direct dispatcher takes `(nr, args[6])`, decodes the native number to the
proxy's common ID, and shares path/descriptor dispatch and pack translation
with SIGSYS. No synthetic ucontext is used. Internal raw syscalls still pass
all installed filters through the existing gateway.

**Red zone:** the replacement call writes below rsp. This initial proof rejects
any function mentioning rsp or modifying the stack, rather than guessing
whether a stack access is safe. This deliberately leaves real stack-framed
wrappers trapped; supporting them needs control-flow-aware stack proofs.

The direct path has per-thread, reusable allocation scopes, including separate
scopes for signal re-entry. A SIGSYS handler pauses interrupted scopes so its
arena cannot reset the direct call's buffers. Thread exit releases scope block
storage; registry nodes persist as in the existing trap-arena registry. Nested
seccomp TRAP from the raw gateway is recognized using the thread's direct
marker and chained to the app's SIGSYS handler, without replaying or bypassing
the denied syscall. Normal signals resume the assembly/C continuation.

### Eligible class and fallback

The backend recognizes primary sites for openat, close, newfstatat, statx,
fstat, readlink/readlinkat, getdents64, lseek and fcntl. For fcntl only GETFD,
SETFD, GETFL, SETFL, DUPFD and DUPFD_CLOEXEC dispatch directly; locking and all
other commands execute a deliberately trapped syscall from the resident slow
entry. Descriptor/pack tracking is shared with the ordinary handler.

Everything unproven stays trapped: stack-framed/red-zone functions, unsupported
decoder opcodes, indirect control flow, absent/ambiguous symbols, FDE-only
ranges, secondary encodings (including musl cancellation), restartable syscall
classes, page boundaries, shared, execute-only or writable executable mappings, anonymous/JIT
pages, unsupported architectures and failed W^X operations. This is a
compatibility/performance layer inside the Android app sandbox, not an
additional security boundary.

### Host coverage and tests

`tests/site-coverage.c` reports actual post-load bytes, rather than estimating
coverage from syscall encodings. The host libc coverage table is recorded with
the measurement results below. In particular, host openat, close and fcntl
contain stack operations and remain trapped. The benchmark uses noinline,
fixed-number assembly wrappers that pass exactly the same scanner; native,
trap and rewrite run identical wrapper code. Generic libc `syscall()` uses a
dynamic syscall number and would not measure the primary encoding at all.

`tests/site-rewrite.sh`, also run by `tests/run.sh`, checks both toggle states:
7/9-byte windows, each syscall class, descriptor duplication, fcntl lock-command
fallback, exec, dlopen/reload/dlclose, file-backed mprotect/munmap, fork,
six-thread hammer, timer signals during proxy execution, nested raw seccomp
TRAP with an app handler, page-straddling windows, interior branch targets,
red-zone use and anonymous executable code that retains its syscall bytes.
No VM or host device passthrough is used. Android binaries are cross-built by
the existing host suite; Android runtime behavior is not claimed here.

The existing `--count` trap/raw summaries retain their SIGSYS meaning; they do
not yet attribute direct-dispatch calls. Use byte coverage and counts-off
benchmarks for this phase.

### Next phases

Broaden x86_64 decoder/stack proofs and add FDE ranges, preserving the trap
fallback for cancellation/restart semantics and app-owned user dispatch. Then
implement i386 on this shared load-time core: AT_SYSINFO/virtualized TCB sysinfo
entry substitution (never patch the vDSO), primary int80 call-table windows,
six-register preservation and non-flat-code-segment fallback. Arm64 follows
with its own branch-only backend and cache maintenance. Anonymous/JIT handling
and runtime discovery/parking belong to a later version, not this one.
