# V3 Linux images in the stub's ART process

Current stages 1–3 implementation and acceptance evidence:
[V3-IN-PROCESS-STAGES-1-3.md](V3-IN-PROCESS-STAGES-1-3.md).
The stage-one text below is historical; its pending fd/VMA/exec items and
permission restriction are superseded by that report. Full Flatpak production
launch still uses ProcessBuilder until stages 4–7 pass.

Stage 1, 2026-10-17. Full Flatpak/Firefox in-process launch does **not** work
in this revision. `V3Launcher` still starts that chain with ProcessBuilder.
The new JNI route runs diagnostic Linux images in the calling stub's main
process, on resident pthreads. It is the required replacement architecture,
not an optional alternative to the child launch. Device results are recorded
in the final report; host success alone is not device verification.

## Inventory of the existing chain

“Handled” below means the **standalone proxy**, not necessarily the new JNI
runner. The runner initially shares ELF loading and thread adoption, not the
entire production syscall dispatcher.

| Dependency on a separate process | Existing implementation | Classification for ART coexistence |
| --- | --- | --- |
| ELF entry, main, argv/envp/auxv | `loader.c` maps ELF/interpreter, constructs Linux entry stack and auxv | Handled by proxy; JNI imports hardware/vDSO auxv from resident `getauxval`, never guesses JNI stack layout |
| execve/execveat and shebang | `proxy_load` replaces user entry context; `state_exec` kernel-execs a peer for cross-class images | Needs change: image ownership, rollback, Linux-thread quiescence, per-image TLS/signal reset; never kernel-exec ART; stage-one rejects exec and ET_EXEC |
| Flatpak Bionic entry and bwrap | Flatpak helper enters Bionic linker, forks/execs bwrap and final ELF | Needs design: remove helper process assumptions and either execute setup as a library or collapse exec hops into image replacement; do not execute Bionic startup again inside ART |
| Namespaces/mounts | Proxy emulates user/mount namespaces and mount table/path resolution | Handled by proxy, needs change to dispatch from adopted threads and keep cwd purely virtual |
| return from main, exit/exit_group, atexit | Linux libc runs its own atexit, then proxy forwards process exit | Needs change: JNI exit gate restores resident FS/stack and returns status for cleanup; production then ends the owning Android process; the diagnostic alone keeps ART alive to verify return; general group shutdown remains |
| Linux pthread creation, TLS, robust lists, clear-child-tid, rseq | Freestanding proxy raw-clones; spike-3 creates resident pthreads and adopts Linux registers/TLS | Needs change: adoption backend now uses thread-local seccomp TRAP, not syscall-user-dispatch; registry is keyed by kernel TID, no libc TCB offsets |
| SIGSYS | Standalone installs one resident action, virtualizes application action and masks | Needs change: preserve previous resident SIGSYS action; route only adopted threads; never allow payload to replace proxy action |
| SIGSEGV/SIGBUS, ART implicit checks, sigchain | Standalone resets signal actions on install/exec | Needs design: per-Linux-image actions with fault routing by thread ownership; retain ART handlers on ART threads; runner does not call `proxy_install` or reset them |
| SIGCHLD and process-directed signals | Standalone action table assumes whole process belongs to Linux | Needs design: child ownership and delivery to Linux execution threads, not arbitrary ART threads |
| Crash containment | Fatal signal kills loader child | Shared-address-space Linux crash may kill stub (accepted cost); report through normal process death/relaunch, no attempt to recover corrupted ART |
| getpid/getppid, /proc/self, prctl | Real kernel identity plus selected proxy emulation | Needs design: getpid must expose stub PID; /proc/self includes ART; virtual exe/cmdline/maps and selected prctl effects need explicit policy |
| File table, fd numbers, CLOEXEC | Proxy tracks virtual pack fds and closes real CLOEXEC fds at exec | Needs design: reserve logical Linux fd namespace, own only Linux descriptions; never close ART Binder/Surface/event fds; fixture output uses passed duplicate fds, no dup2 on ART stdio |
| cwd, umask, rlimits | `vfs_chdir` also changes real cwd; other changes can be process-wide | Needs change: virtual cwd/umask/limits, apply creation masks in proxy, reject unsafe global operations; no JNI chdir/setenv/setrlimit |
| mmap hints/MAP_FIXED, brk | Loader protects only its fixed resident range; standalone brk belongs to process | Needs change: track ART versus Linux VMAs, reject foreign unmap/remap/fixed collisions; runner accepts only PIE, freezes Linux brk so libc falls back to mmap |
| fork/vfork, children | Standalone fork paths and adoption spike support children | Needs design: same-app Android child-process broker, Linux state snapshot and wait/SIGCHLD transport; never native-fork ART; initial JNI slice rejects fork/vfork |
| ABI changes | Separate i386 peer loader uses kernel exec | Needs design: cannot switch ART process to i386; explicitly reject in JNI until same-process compat execution is designed |

## Entry and ownership

`InProcessLauncher.nativeRunInProcess(argv, env, fds)` copies Java arrays,
duplicates the supplied output descriptors, and calls the C runner. No Java
object is retained across Linux execution. A mutex permits one live Linux
image per stub. A dedicated resident pthread loads the PIE image and its
interpreter, constructs its entry stack, saves the resident TLS/stack and
enters Linux `_start`. Linux libc alone owns its main/argv/envp and atexit.
The Android application, Binder connection, compositor and Surfaces remain
in that same PID and address space.

Only that Linux thread and adopted workers receive a new seccomp filter;
there is no TSYNC. The inherited zygote filter remains underneath. TRAP takes
precedence over inherited ERRNO for payload syscalls, but raw gateways must
still use calls allowed by the inherited filter. The filter permits the
resident raw gateway and resident libc executable mapping. This is routing,
not a security boundary: the existing stub UID/SELinux sandbox is the boundary.
Resident libc needs native clone continuations and its own pthread TLS.
On Bionic, the libc/ART SIGSYS trampoline can touch FS before our handler
restores it; the JNI runner therefore installs SIGSYS directly and never
changes ART SIGSEGV/SIGBUS actions. This still needs an audit against future
ART SIGSYS ownership changes. An
application clone is decoded from the kernel ABI and fulfilled by a resident
pthread; the new worker then adopts Linux TLS/registers. Signal handlers and
FS switching must never invoke compiler TLS, stack protectors or lazy PLT
binding while application FS is installed.

The new backend is compiled under `MATON_IN_PROCESS`; the existing spike and
standalone proxy retain their behaviour. The process-wide SIGSYS action is
installed with the kernel rt_sigaction ABI and a raw restorer, preserves the
prior kernel action, and routes
unowned threads to it. SIGSEGV/SIGBUS are untouched in this first slice. Signal
mask changes retain deliverable SIGSYS. This must be tested with ART's sigchain
on device; host glibc is not an adequate substitute.

A clean exit gates through sigreturn to restore resident FS/stack, performs
Linux robust-list/clear-child-tid cleanup, lets resident pthread cleanup run,
and returns the Linux exit status to Java for cleanup. Production must then
terminate this Android process; only diagnostics retain it for responsiveness
checks. Initial images require joined
workers. Unjoined-worker exit_group is **not yet safe**: group cancellation,
wait-for-quiescence and resource release are prerequisites for arbitrary apps.
The initial runner rejects fixed-address ET_EXEC to protect ART mappings and
releases the main/interpreter/entry-stack mappings after the entry returns.
Payload-owned allocator/TLS mappings and proxy diagnostic bookkeeping still
need a complete per-session resource ledger before long-lived repeat use.

## Exec and Flatpak plan

1. Factor a callable production proxy dispatcher with explicit Linux session
   state. Connect it to the adoption trap so all legacy syscall translations,
   VFS, packed files, DNS and namespaces work under the zygote filter. Reject
   process-wide mutations until their virtual equivalents exist. Do not invoke
   the standalone `proxy_install`, `exec_signals` or global CLOEXEC sweep.
2. Add logical fd ownership and VMA ownership. Exec stages an image, validates
   it, quiesces only Linux workers, disposes only old Linux-owned resources,
   resets only Linux signal/TLS state, closes only Linux CLOEXEC descriptions,
   and replaces the Linux entry context. ART keeps running. Failed validation
   resumes the old Linux caller; no kernel exec in the parent.
3. Convert Flatpak/Bionic helper startup to an in-process callable setup path
   (preferred), using the existing Flatpak planning/setup code. Retain stock
   Flatpak decisions and bwrap mount arguments, translated into the per-session
   virtual mount table. Remove supervision forks and collapse the final exec
   into userspace replacement. If a Bionic ELF hop is retained, design its
   linker/TLS/constructor isolation explicitly; it cannot reuse ART's process
   startup assumptions. No feature is considered complete by keeping the old
   child chain.
4. Implement Linux group exit, signal action/delivery ownership, resource
   teardown and repeated-session tests. Test ART GC, Binder, looper, Surfaces,
   Linux fault death and relaunch, including exit from a worker.
5. Every app-requested Linux process (fork/clone without CLONE_VM,
   posix_spawn, vfork+exec) must run inside another zygote-created Android
   process of the same stub app. Generate private service slots with
   `android:process=":linux_child_N"`; a same-UID authenticated broker assigns
   a free slot, starts its service and transfers a bootstrap through Binder
   descriptors. A finite manifest slot pool needs explicit exhaustion behavior
   (EAGAIN), never a native-fork fallback. Each service owns ART, Binder,
   compositor and its per-window Surfaces; GPU/content children can use the
   direct Android window path too. No inherited Java object or Surface pointer
   crosses processes. Surface/window IDs and descriptors cross authenticated
   IPC where required.

   Spawn/exec bootstraps carry argv/env, virtual mounts/cwd/umask/limits,
   logical fd descriptions and shared mapping handles. General fork is harder:
   another zygote process has its own ASLR/ART mappings, so restoring Linux
   VMAs/registers at identical addresses needs a conflict-checked snapshot
   protocol and shared-file-description/CoW semantics. This is unresolved,
   mandatory work; native fork is not an acceptable shortcut. vfork suspends
   the Linux caller until child exec/exit acknowledgment. Maintain a broker
   parent/child registry with virtual Linux identity, exit status, wait/waitid
   and SIGCHLD delivery; Binder death must also report crashes and unexpected
   process loss. Process-directed Linux signals route to the target session.
   Tests must cover orphan/reparent behavior and parent death.

   On clean Linux exit, quiesce its Linux threads, release windows/resources,
   report status to its broker parent, then terminate the owning Android
   process. Crashes may terminate it immediately; Binder death completes the
   parent's wait state. No production stub/service lingers or restarts after
   payload death. The stage-one diagnostic intentionally returns to live ART
   for two runs and looper checks before this final lifecycle step: it proves
   a cleanup gate, not production exit policy.
6. Switch `V3Launcher` to JNI after those invariants pass and keep the old
   ProcessBuilder launch only behind an explicitly named temporary development
   switch during transition. Delete that path and the switch once Flatpak and
   Firefox in-process acceptance passes. Stage-one fixtures are not grounds
   for silently switching arbitrary Flatpaks to an incomplete dispatcher.

## Vulkan forwarder

Linux rendering in the parent can call resident libgui with adopted resident
TLS and use the stub's actual per-window Surface. This removes the current
cross-process Surface limitation. It does not by itself provide Vulkan WSI:
window-to-Surface lifetime, acquire/present synchronization, buffer ownership,
resize/destruction and resident callback entry still need their forwarder
implementation. Each Android child process owns its own libgui/Surface path; cross-process GPU/window ownership still requires explicit authenticated fd/ID transport.

## Tests and diagnostic switch

Host: `linux/compat-proxy/inprocess/test.sh EXTERNAL_OUTPUT` runs static PIE
and glibc fixtures twice, each reading argv/env, printing PID, creating and
joining a pthread, and returning normally. The resident checks status and PID.
`build-fixtures.py` additionally packages a hash-pinned Ubuntu glibc
2.39-0ubuntu8.9 loader/libc pair solely for the device diagnostic; it is not a
Flatpak runtime pin. Its fixture's DT_NEEDED is padded in place to name
`libt.so` for APK extraction. The pair is LGPL-2.1-or-later, unmodified apart
from packaging filenames; corresponding Ubuntu glibc source is required for
redistribution. These diagnostic artifacts and the host-toolchain pin should
be removed from production packaging after this stage.

Device (only after the assigned VM is free): update the signed subsystem APK,
then launch the shared StubActivity with boolean intent extra
`org.matonos.linux.IN_PROCESS_SELF_TEST=true`. This runs only packaged fixture
paths, never intent-supplied executables. Both modes run twice via JNI; logcat
`MatonInProcess` records the Linux PID, resident PID, status and main-looper
callback. Start a second activity in the same process and check dumpsys
activity and PID continuity. The shared activity works in a generated stub as
well as the subsystem's own package; acceptance requires a generated stub's
main process, not an isolated host-service process.

Full host proxy suite: run `linux/compat-proxy/tests/run.sh` in a disposable
copy beside packs. Repo tests and `./build.sh` use the assigned external output.
No AOSP, kernel, shared policy or bridge change is needed for this slice.

## Verified slice, 2026-10-17

The signed APK built with `./build.sh` passes static PIE and the pinned glibc
loader/libc fixture through the subsystem APK's shared `StubActivity` in its
main ART process on localhost:5556. Two independent diagnostic launches each
run static + glibc twice. Linux prints PID 3741, argv `fixture`, environment
`present`, and worker output; Java reports status 0 and main-looper callbacks
in PID 3741 after both launches. `dumpsys activity activities` still shows that
same process with resumed StubActivity windows. This verifies a resident JNI
slice, not a Flatpak launch or production exit lifecycle.

No generated stub is installed in this VM. Verification under a generated
stub's separate UID is pending permission to install a diagnostic APK (the VM
brief permits only subsystem APK updates). A signed, generated diagnostic stub
is prepared outside the repository; no additional package has been installed.

The WIP SIGSYS registration was unsafe on device: the inherited libc/ART
signal trampoline faulted at FS+0x28 before the proxy handler. Direct kernel
SIGSYS registration fixes this. Linker-startup FS=0 is also Bionic's resident
FS=0, so it cannot identify resident calls. The filter exempts resident libc
by instruction address instead. Legacy access/open/stat/readlink calls use
modern at-family syscalls; any retry still trapped by the inherited filter
returns ENOSYS rather than recursing to signal-stack exhaustion. These few
translations are diagnostic coverage; the production dispatcher integration
listed above remains mandatory.

`inprocess/test.sh` now also layers an inherited TRAP filter over access and
getpriority. Access must translate successfully; unsupported getpriority must
return without recursive trapping. Static/glibc, status 37, repeat invocation,
ART fault-action preservation, fixed-address rejection and cross-class rejection
pass on host. The standalone proxy suite and repository suites pass; the signed
APK passes alignment, signature and native dependency-closure checks. Device
logs and complete build output live in the assigned external output directory.

## Generated-stub acceptance, 2026-10-17

The prepared diagnostic APK was installed on localhost:5556 under
`flatpak.org.matonos.InProcessTest`, UID 10114. Static PIE and pinned glibc
each ran twice in its main ART process, PID 4388, with status zero and a
responsive main looper. A second new-task activity repeats these assertions
in the same process. Host APK PID 3741 has UID 10078; this is a distinct stub
identity. Evidence: `v3-inproc-out/ownuid-stage1.log` and
`ownuid-stage1-activities.txt` in the assigned external build directory.

The user decision of 2026-10-17 supersedes the earlier native-fork fallback:
plain fork is emulated in another zygote-created same-app process, with eager
mapping/state transfer at identical reserved addresses. Native fork of ART is
forbidden. This and Android spawn/exec children remain stage 6.

Current incomplete stages 4–5 work and validation:
[V3-IN-PROCESS-STAGES-4-5.md](V3-IN-PROCESS-STAGES-4-5.md).
