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
