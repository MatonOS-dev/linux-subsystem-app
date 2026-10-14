# Round 30 run 3 — i386 resident loader

P1 passes. Dynamic P2 fixtures pass, but full P2 acceptance and P3 remain
unfinished. No external blocker was found. Cross-class exec remains ENOEXEC;
that is missing handoff code, not a reason to treat the task as complete.

All code changes are in this worktree's linux/compat-proxy. No commits, adb,
image builds, installs or APK changes. Suggested checkpoint message:
`compat-proxy: add freestanding i386 backend and static/dynamic compat tests`.

## Implementation

- Fixed-width six-word arguments; backend signal masks/altstack/exec-context
  hooks and raw/clone BPF PC hooks. Eight-byte signal masks fix compat stack
  corruption in fd-name/DNS bookkeeping.
- i386 number table, assembly, ELF32 stack/header/auxv/entry and low resident
  address. Kernel GS TLS remains active during handlers; resident exec resets
  saved GS/FS as well as current GS. mmap2/64-bit offsets and _llseek replay.
- A separately generated x86 app policy from the checked-out AOSP inputs.
  This is still single-architecture per loader, not the required dual filter.
- socketcall and ipc multiplexer decode, shmat output ABI, semctl indirect
  union and IPC_64 handling, direct IPC, semtimedop_time64.
- Canonical stat/socket/time/IPC adapters in arch/, stat/statx/message ABI
  assertions, common fixed termios2 wire format, IPC v2 records with explicit
  fixed sizes, 64-bit times and message types, high time-word conversion.
- Both build scripts build an ELF32 loader, using the NDK sysroot/builtins
  without multilib development headers. Host payload fixtures dynamically
  link the installed i386 glibc 2.39. No upstream implementation was copied.

## Validation

- Complete tests/run.sh: x86_64 regression suite and added i386 fixtures pass.
- P1: native and proxied static stat64/open/read/write, translated leaf map,
  copied i386 app policy.
- Dynamic: glibc TLS/thread/futex/signal startup, infinite poll, getdents64 and
  same-class exec; sockets/messages/SCM_RIGHTS; real Unix DNS conversion;
  pty/termios2; ipc(117) semget/semctl/semop/shmat/shmdt/shmctl;
  semtimedop_time64 timeout; direct semget/semctl control-buffer conversion.
- Isolated NDK build-android.sh: both x86_64 and i386 artifacts compile.
- git diff --check passes. Preflight fails on the isolated worktree's existing
  missing image APK/native prebuilts; no shared build requested.

Evidence: build/r3-final-tests.log, build/r3-ndk.log, build/r3-preflight.log,
build/i386-p1/*.log and *.out. Artifacts: build/linux-layer-v3-i386 and
build/android/libmatonos-v3-loader{,-i386}.so.

## Remaining work

P3 needs dual-arch inherited filters/stable gateways, trusted peer selection,
a private authenticated versioned state FD, and full state import/export for
VFS, inode/temp ownership, private/CLOEXEC/trace fds, network/DNS and the IPC
namespace/SEM_UNDO identity. Mixed-class IPC and bidirectional exec have not
been tested. No host APK packaging was changed.

Full arch-neutral refactoring still needs legacy stat/old mmap and remaining
time/time64/IPC-info/fd-set adapters. P2 needs broader resolver/TCP/DNS timeout,
IPC message queues, fork/exec inheritance and signal-frame/FP state coverage.
No Steam/Wine or enforcing-device support is claimed. README contains build
commands, explicit gaps and fresh-image device steps for all three target PCs.
No shared-file or kernel change requested in this checkpoint.
