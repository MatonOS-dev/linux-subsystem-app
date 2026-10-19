# Syscall-site rewrite proposal and proxy measurements

This is a design only. The proxy changes in this branch add optional counters;
they do not rewrite syscall sites.

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

## Syscall-site rewrite design

### Mapping and W^X

The first `SIGSYS` is the authoritative site discovery event. Accept it only
when `si_code == SYS_SECCOMP`, the decoded syscall is supported by the proxy,
and the architecture backend confirms that the reported call address contains
the expected syscall opcode. Do not infer a site from the syscall number
alone.

For a private executable file mapping, make an anonymous page copy at the same
virtual address: allocate a separate anonymous RW page, copy the original
page bytes, apply the branch patch and trampoline data, change the copy to RX,
then replace the old page with `MAP_FIXED`. The file-backed page is never
writable, so this does not request SELinux `execmod`; the new executable page
has anonymous provenance. Preserve the original file bytes in the copy except
for the patched window. Keep the original VMA start, end, and
protections so normal `munmap` and `dlclose` ranges still remove the mapping.

There is an unavoidable `/proc/self/maps` difference: the replaced page is
anonymous and loses its original pathname/device/inode entry. An anon name
such as `[anon:matonos-syscall-rewrite:<object>:<page>]` can identify it, but
cannot truthfully preserve file identity. For callers that require file-backed
identity (mapping scanners, self-inspection, profilers), skip rewriting that
object. Do not use a writable alias of the executable file or a memfd-backed
RX mapping as a workaround; SELinux labeling and file identity make those
approaches less reliable. Anonymous mappings are released by the original
`munmap`/`dlclose` operation. Track the page only to avoid duplicate patches;
drop that record when its address range is unmapped or replaced.

### x86_64 site encoding

`syscall` is two bytes (`0f 05`), too short for any direct jump. A fixed
low-address trampoline (zpoline's `call *%rax`) is not available:
`mmap_min_addr` forbids mapping page 0. The design therefore patches the
instruction that loads the syscall number together with the `syscall`.

**Primary encoding: immediate number, adjacent load (7-byte window).**

```
before:  b8 nn nn nn nn   mov $nr, %eax      ; 5 bytes
         0f 05            syscall            ; 2 bytes
after:   b8 ss ss ss ss   mov $stub, %eax    ; 5 bytes
         ff e0            jmp *%rax          ; 2 bytes
```

- The window is exactly the original two instructions; nothing else moves.
- Clobbering `%rax` is free: the syscall overwrites it with the result anyway
  and no code may rely on it across `syscall`.
- `stub` is a per-site stub. It knows `nr` (it was the immediate), runs the
  common emulation entry, writes the result to `%rax` and jumps to the
  instruction after the window with a `jmp rel32` or an absolute jump.
- `mov $imm32,%eax` zero-extends, so all stubs live below 4 GiB
  (`MAP_32BIT` or a low hint; `mmap_min_addr` only excludes the first 32 KiB).
  One global stub area serves every library, wherever it is mapped: no
  per-library near allocation.
- The indirect jump goes to a fixed target per site and predicts perfectly.
  Stubs start with `endbr64` so the scheme stays valid if user-space IBT is
  enabled later.
- A `mov $nr,%rax` (`48 c7 c0 imm32`, 7 bytes) adjacent load gives a 9-byte
  window; use the same stub with two trailing NOPs.

**Secondary encoding: number in a register (`jmp rel32`).** When the number is
not an immediate (`mov %rsi,%rax`, Go's stack load, musl's cancellation
routine), overwriting `%rax` would lose it. Replace a relocatable preceding
instruction plus `syscall` (at least 5 bytes) with `jmp rel32` to a per-site
stub within ±2 GiB, pad with NOPs, run the moved instruction in the stub, and
read the number from `%rax` at run time. Only relocatable instructions with no
RIP-relative operand (or with a fixed-up displacement) qualify.

**Branch rule.** No branch may target any byte inside the window except its
first byte. A target equal to the window start is fine (retry loops jump back
to the `mov`); a target on the `syscall` itself is not. Check direct branch
targets in the containing function; indirect targets cannot be proven, so
only patch recognised wrapper shapes (below) and keep the trap fallback.

**Everything else stays on the trap:** `xor %eax,%eax; syscall` (4 bytes;
glibc's `read` wrappers), non-adjacent number loads with intervening
instructions, unknown shapes, page-straddling windows.

#### Measured coverage (static, 2026-10-19)

glibc from the Freedesktop 26.08 runtime (`libc.so.6`, 484 sites):

| Shape before `syscall` | Sites | Share |
|---|---:|---:|
| `mov $nr,%eax` adjacent (primary) | 409 | 84.5% |
| `mov $nr,%eax` earlier, non-adjacent | 63 | 13.0% |
| `mov $nr,%rax` adjacent (9-byte window) | 1 | 0.2% |
| `xor %eax,%eax` (`read`, nr 0) | 2 | 0.4% |
| number from a register | 3 | 0.6% |
| other | 6 | 1.2% |

Of the 409 adjacent sites, 13 are direct branch targets — all at the `mov`
(window start), none at the `syscall`. The hot wrappers from the trap profile
are all adjacent-immediate: `openat` (4 sites), `newfstatat` (2), `statx`,
`readlink`, `readlinkat`, `getdents64`, `lseek`, `fcntl`, most of `close`,
`mmap`, `fstat`. Only `read` uses the `xor` shape.

musl 1.2.x from Alpine 3.24 (`ld-musl-x86_64.so.1`, 508 sites):

| Shape before `syscall` | Sites | Share |
|---|---:|---:|
| `mov $nr,%eax` adjacent | 290 | 57.1% |
| `mov $nr,%eax` earlier, non-adjacent | 192 | 37.8% |
| number from a register | 8 | 1.6% |
| `mov $nr,%rax` adjacent | 1 | 0.2% |
| other | 17 | 3.3% |

musl's syscalls are inline asm with an `"a"(n)` operand, so the compiler
schedules the load freely (hence the 38%). Its hot path is different, though:
every cancellable call (`read`, `write`, `openat`, `close`, `poll`, locking
`fcntl`, ...) goes through one routine, `__syscall_cp_asm`, with one call site:

```
__cp_begin:  mov (%rdi),%eax ; test %eax,%eax ; jne __cp_cancel
             mov %rsi,%rax                 ; number from a register
             ... argument shuffle ...
             mov %r11,0x8(%rsp)            ; 5 bytes  -> relocated
             syscall                       ; 2 bytes
__cp_end:    ret
```

This site takes the secondary encoding (`jmp rel32` over
`mov %r11,0x8(%rsp); syscall`). One patch covers most of musl's hot traffic.

**musl cancellation rule.** musl's cancel handler acts on `pthread_cancel`
during a blocking syscall only if the interrupted IP lies in
`[__cp_begin, __cp_end)`. After patching, the blocking call runs from the
stub, outside that range, so a cancel would only be noticed after the call
returns. Before patching this site, the stub must preserve cancellation-point
semantics (e.g. when a signal interrupts the stub's blocking raw syscall,
rewrite the saved IP to the original `syscall` site so musl's handler sees it
in range), proven by a test that cancels a thread blocked in `read` under
musl. Until then this one site stays on the trap. The current SIGSYS path has
the same property in principle and gets the same test.

bionic's generated syscall stubs are adjacent `mov $nr,%eax; syscall` by
construction (only relevant if bionic payload code is ever trapped).

### Other architectures

One mechanism on every architecture: the core (first-trap discovery, stub
record, emulation entry, thread parking, signal/restart handling, SUD
coexistence, trap fallback) is shared; a backend only answers "is this a
patchable site", "write the jump", "save/restore registers" and "flush the
icache". x86_64 writes a 7-byte window (`mov $stub,%eax; jmp *%rax`); arm64
writes one `b stub`. (`adrp x16,stub; br x16` was considered for arm64 to mirror
x86_64, but it needs a two-instruction window, page-aligned stubs or a third
`add`, an `x16` liveness check and non-atomic patching; `b` avoids all of that.)

- **arm64 (decided):** plain branch replacement only. `svc #0` (4 B) becomes
  `b stub` (4 B, ±128 MiB) one for one; no instruction relocation, no other
  encodings. The per-site stub saves state, emulates (`x8` = nr, `x0`–`x5`
  args), returns in `x0`, then `b site+4`. No `bl` (it would clobber `x30`).
  Concurrent modification of `SVC`→`B` is architecturally permitted; publish
  with `dc cvau`/`ic ivau`/`isb`. BTI does not apply to direct branches. Sites
  whose stub cannot be placed within ±128 MiB stay on the trap.
- **riscv64 (decided):** no site rewriting for now; every syscall stays on
  the SIGSYS trap path. (Reasons: `jal` reaches only ±1 MiB, longer reach
  needs a scratch register, `ecall` may be only 2-byte aligned with the C
  extension, and cross-hart icache sync needs an extra syscall.)

Order: x86_64 (shipping), then arm64.

### Concurrency and process semantics

- **Patch races:** publishing an RX anonymous page is atomic at the mapping
  level, but another thread may already be executing the old page or may enter
  the site during publication. Before replacement, signal every thread in the
  mm with a reserved real-time signal, park them in a minimal handler, and
  verify their saved PCs are outside the page being replaced. Retry briefly;
  if any thread cannot park or its PC is in the page, skip the patch. Do not
  spin in an async signal handler while another thread holds proxy/VFS locks.
  Publish the stub before making the replacement page
  visible; use release/acquire state and the kernel mapping/TLB synchronization
  from `mmap(MAP_FIXED)`.
- **Signals and `sigreturn`:** a signal can arrive while a trampoline saves
  registers, calls the dispatcher, or returns. Keep per-thread trampoline
  state and a versioned signal-frame marker. The proxy's signal chaining must
  preserve the interrupted trampoline PC and saved registers; `sigreturn`
  resumes the same trampoline continuation exactly once. If the app's own
  SIGSYS handler or `PR_SET_SYSCALL_USER_DISPATCH` owns the event, do not
  rewrite it. Preserve existing `SA_RESETHAND`, mask, and altstack behavior.
- **Clone, vfork, and fork:** `CLONE_VM` shares rewritten text and the patch
  registry, so it must join the thread parking protocol before it can enter
  user code. A `vfork` child shares both text and the parent's stack; never
  patch from the child before exec. Keep the trap path for vfork children and
  for clone variants that cannot safely register. Forked processes inherit
  the patched anonymous page by COW, but get a private registry after fork.
- **Restartable calls:** direct dispatch must reproduce the current handler's
  `-ERESTARTSYS` behavior, including kernel restart after `sigreturn`,
  `SA_RESTART`, and non-restarting handlers. The trampoline must retain the
  original syscall PC and arguments so the signal path can retry or return
  `EINTR` exactly once. Until that is proven, do not rewrite restartable
  syscalls; retain their SIGSYS site.
- **Seccomp inside emulation:** a raw syscall issued by the dispatcher still
  passes through every installed seccomp filter. An internal-dispatch marker
  lets the SIGSYS handler recognize and handle a nested trap from a trampoline
  without recursively rewriting the same site. Never bypass a stacked filter
  or turn a denied raw syscall into an allowed one. If the filter cannot be
  honored without recursion, return through the existing trap path.
- **Wine/user-dispatch:** Wine's own SUD selector and SIGSYS behavior remain
  application-owned. Only `SYS_SECCOMP` events attributed to a known syscall
  opcode and outside an app SUD interval are eligible. Keep the current SUD
  signal chaining path intact.
- **JIT and self-checking code:** permit anonymous JIT pages only when their
  owner opts in and the page is stable/executable at the trap. Rewriting
  changes code bytes and can break checksums, code signing, JIT invalidation,
  or debugger breakpoints. Detect common JIT write/execute transitions and
  skip them by default; allow explicit opt-in per process/object. Revalidate
  that the page contents still match the captured copy before replacement.

### Unpatchable cases and rollout

Keep the signal fallback for shared writable/executable mappings, text pages
whose file identity is required, windows straddling a page boundary, shapes
other than the recognised wrappers, unknown prefix/decoder state, overlapping instruction streams, code
that changes after discovery, unsupported syscall IDs, app-owned SUD/SIGSYS,
unsafe thread states, vfork children, and restartable calls until restart
semantics are validated. Start disabled by default behind an environment or
debug property. Enable one architecture and one syscall class at a time;
compare counts-off wall timings and counts-on attribution on the same refs.

