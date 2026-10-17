# Resident Linux sessions: stages 1–3, 2026-10-17

Full Flatpak and Firefox in-process launch **does not work yet**. Production
`V3Launcher` still uses ProcessBuilder. Do not switch arbitrary Flatpaks to
this diagnostic dispatcher. Callable Flatpak setup, production group exit,
signal delivery, Android child processes and Firefox are stages 4–7.

## What this change does

The generated diagnostic stub runs static PIE, pinned glibc, and a userspace
exec chain inside its own main zygote-created ART process. The Linux payload
uses resident pthreads in that same address space. ART, Binder and Android
windows are retained; the diagnostic returns to ART to prove that retention.
Production process termination after Linux exit remains stage 5.

`inprocess/session.c` explicitly owns the Linux descriptor namespace, mapping
ledger, virtual umask, resource-limit values and Linux signal-action records.
One session is allowed per process by the runner mutex. Existing internal
VFS/pack/DNS tables remain singleton implementation state, reset at session
boundaries. They are not independently reentrant sessions. Trusted setup APIs
bind host paths, mount immutable packs and configure the DNS transport.

`proxy_resident_dispatch` calls the production translation dispatcher without
`proxy_install`, standalone signal installation, kernel exec or a process-wide
CLOEXEC sweep. The resident session intercepts ownership-sensitive operations
and only forwards its explicit supported syscall set. Namespace/mount and VFS
operations, packed file I/O and DNS socket conversion use the existing code.
Linux chdir/fchdir change only virtual cwd, never ART's cwd. Unsupported calls
return ENOSYS rather than passing unclassified fd/memory operations to the host.

Linux fd numbers map to owned host descriptions. Linux CLOEXEC is independent
of the backing descriptor's kernel flag. Syscalls pin descriptions until they
finish, so concurrent close cannot recycle a backing number into an ART fd
while that syscall still uses it. Pack and DNS aliases use production tracking.
The session closes only its own descriptions, including at exec.

Initial ELF/interpreter/stack reservations and Linux mmap allocations enter an
owned VMA ledger. Fixed replacement, unmap, mprotect and madvise require complete
ownership. Partial unmaps split the ledger. Metadata is reserved before changing
kernel mappings, so allocation failure cannot leave stale ownership over a hole
subsequently reused by ART. ET_EXEC and cross-class images remain rejected.

Exec loads and validates the new PIE/interpreter/entry stack before changing
the old image. Failure returns to the old caller. A serialized successful exec
quiesces only adopted Linux threads, including a CPU-spinning thread and one
blocked in futex. SIGSYS wakeups are bounded per thread to prevent SA_NODEFER
signal-stack recursion; active dispatcher calls finish their cleanup before
the thread takes the resident exit gate. Registry reservations count as live
until startup finishes or fails. No ART thread is signalled or cancelled.

After quiescence, exec unmaps only the old Linux ledger, closes only Linux
CLOEXEC descriptions, resets Linux TLS/robust-list/clear-child-tid/signal-action
and FP state, and replaces the Linux signal frame's entry and stack. Virtual
cwd, umask, limits and non-CLOEXEC descriptions survive. The kernel never execs
ART. Diagnostics deliberately keep ART alive after the final payload exits.

## Validation

All outputs and evidence are under the assigned external
`/home/hanro50/matonos/build/v3-inproc-out` directory.

- `ownuid-stage1.log`: two generated-stub activity launches, each static/glibc
  twice, PID 4388, UID 10114 (distinct from the host APK UID 10078/PID 3741).
- `session-host.log`: repeated static/glibc runs, inherited seccomp TRAP,
  expected status 37, fixed/cross-class rejection, glibc→glibc→static exec,
  failed-exec rollback, blocking and spinning worker quiescence, argv/env/PID,
  Linux CLOEXEC/non-CLOEXEC checks, resident CLOEXEC fd/VMA/cwd/umask/rlimit
  sentinels and ART fault-action preservation. Also real callable VFS bind,
  STORED ZIP/index slice reads and shared offsets, Unix DNS query/reply and
  repeated session teardown.
- `stage23-proxy-suite.log`: full standalone proxy regression suite, exit 0,
  including mixed-class exec, IPC, Android packaging, real OSTree packs and
  loader mapping fallbacks. Run in a disposable external source mirror.
- `session-apk-build.log`: repository tests, Android C/Java build, APK signing,
  16 KiB alignment and 84-library dependency closure through `./build.sh`.
- `stage3-device.log` and `stage3-device-activities.txt`: final signed APK in
  the generated stub on localhost:5556; repeated static/glibc/exec modes,
  same Linux/ART PID, status zero and main-looper callbacks. Exact final hash
  and PID are recorded with the final verification below.

The glibc chain explicitly invokes the packaged glibc loader with its packaged
library directory, execs that loader with a second glibc payload, then execs the
static PIE. Both execs use the resident path. The test-only glibc pin/provenance
and redistribution constraints are unchanged from V3-IN-PROCESS.md.

A host update left an old shared-library APK path in the already-installed
stub's PackageManager metadata during testing. Reinstalling the diagnostic
stub refreshed that path; force-stop and a fresh diagnostic activity ensured
new code was loaded. Starting an old normal launch task also exposed a
broker `g_libintl_gettext` link error. No broker or shared-library
loader changes are included. Diagnostic activities bypass that production
runtime. The VM's system process restarted during this interval; attribution
is not established. Final verification must follow the restart.

## Deliberate limits

This is an acceptance slice, not full Linux compatibility. The supported
syscall set is explicit. Pipe/dup2/dup3, poll/epoll translation, SCM_RIGHTS,
logical `/proc/self/fd` views, mremap and general prctl are not implemented in
the resident adapter. Do not bypass its whitelist to enable an app.

RLIMIT_NOFILE is enforced in the logical namespace. Other limits are virtual
stored values, not complete CPU/address-space/file-size accounting. File
creation applies the virtual umask before the kernel's unchanged resident
umask; permissions the resident mask removes cannot yet be restored by this
adapter. Complete creation/limit semantics need further compatibility work.

Linux signal actions are recorded/reset without modifying ART handlers;
arbitrary Linux signal delivery is stage 5. Arbitrary exit_group with live
workers is still outside this diagnostic contract. Tests join ordinary workers
and explicitly quiesce workers for exec. The later 2026-10-17 user decision requires eager plain-fork emulation
in a same-app Android process; native fork of ART is forbidden. That and
fork+exec/spawn through same-app Android processes remain stage 6. No child
broker, wait/SIGCHLD transport or production process death is claimed.

The trusted session configuration APIs are native entry points. The current
Java diagnostic uses no Flatpak mount plan. Stage 4 must connect callable
Flatpak setup to them; it must not retain ProcessBuilder as the final design.

## Reproduce / real hardware

1. Set the repository README build environment and the assigned external output;
   put CMake on PATH and Ninja in its own directory containing only its symlink.
2. Run `bash linux/compat-proxy/inprocess/test.sh EXTERNAL_OUTPUT`.
3. Run `./build.sh` with the existing external signing key; never copy another
   task's dependency build/stage directories.
4. Install the signed host APK and generated diagnostic stub on the assigned
   device. Force-stop the diagnostic package after an update. Start
   `flatpak.org.matonos.InProcessTest/org.matonos.linux.stub.StubActivity` with
   boolean `org.matonos.linux.IN_PROCESS_SELF_TEST=true` and a unique string
   `org.matonos.linux.IN_PROCESS_TEST_ID`. For a second activity in the same
   process use intent flags `0x18000000` and another unique id.
5. Check MatonInProcess logs: static, glibc and exec each twice, all status 0,
   matching payload/ART PID and a main-looper callback. Confirm the package's
   own UID/main ProcessRecord and resumed activity with dumpsys.
6. Repeat the diagnostic on Ryzen 5800X/RX 6600, Surface Pro 3 and HP ProDesk
   600 G1. This run used QEMU only; real hardware is not claimed. This slice
   does not depend on physical Wi-Fi, audio or GPU devices.

No AOSP, kernel, SELinux, system bridge, shared build file or other worktree
change is needed. No push or merge is part of this run.

## Final verified artifact

Native/session commit: `4e98290`; identity evidence: `f085923`. Final signed
APK SHA-256:
`cce155f9c665c6f080e7445f302fb39d686788c523c8c84bc0363ba61b523ab2`.
On localhost:5556, UID 10114/PID 13277, two fresh diagnostic activities each
ran static, glibc and exec twice. All twelve mode results were zero; all four
exec chains asserted argv/env/fd/PID preservation; both activities posted
main-looper callbacks. The matching main-process activity record is retained
in the external evidence directory. This is after the observed VM system
process restart and uses the final rebuilt artifact.

The host APK update and diagnostic reinstall remain on the assigned VM. No
files were adb-pushed, no system files were modified, and no test VM was
started or killed. No other worktree was modified, and nothing was pushed
or merged.
