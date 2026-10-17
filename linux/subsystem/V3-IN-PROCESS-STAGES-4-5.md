# Stages 4–5 progress, 2026-10-17 — acceptance NOT complete

**What does not work:** Flatpak setup is not callable in-process. No installed
Flatpak has been run in the stub's main process by this work. Production
`V3Launcher` still uses ProcessBuilder; that deviation remains unresolved.
Stage 4 is not implemented or accepted. Stage 5 is partial: arbitrary Linux
signal handlers, process-directed signal routing and production death/status
reporting are not implemented. Real compositor/Binder/Surface teardown is not
accepted by the diagnostic results below. No stage 6 or 7 completion is claimed.

## Changes

`471bce6` adds resident group shutdown: the first Linux exit_group caller owns
status, wakes/quiesces adopted Linux siblings and takes the resident exit gate.
Competing exit callers leave instead of waiting for each other. Main-thread
SYS_exit waits for remaining Linux threads before the runner frees the session's
VMAs/fds. An exec interrupted by group shutdown discards its staged image before
leaving. Successful exec now unbinds the old Linux FS-base map key before setting
FS to zero; later TLS installation rekeys through arch_prctl as before.

`d1d8601` fixes a teardown race discovered by repeated competing exits. A stopper
can already have read a tid when its target removes ownership. A late SIGSYS
then looked unowned and chained to the resident's previous action, killing the
process. Exiting resident pthreads now block further wakeups after consuming the
Linux exit signal frame, before removing their identity/altstack. Pending wakeups
and the thread-local filter die with that pthread. The standalone backend is
unchanged. The test suite now repeats competing group exits.

`9879c26` adds fixed-payload lifecycle diagnostics, selected separately from the
existing keep-ART-alive diagnostic. Clean return/group shutdown records status,
closes any existing per-app runtime, removes the task on the main looper, then
kills its own Android process. This is diagnostic-only, not production launch.
A fault kills the owning ART process; ActivityManager may recreate its activity.
The persisted attempt id makes that replacement remove the task and terminate,
instead of leaving an idle stub. This does not implement general crash reporting
or attempt to recover a corrupted address space. Missing/unknown diagnostic
parameters do not accept executable paths from intents.

## Validation

All evidence is under `/home/hanro50/matonos/build/v3-inproc-out`.
The final signed APK SHA-256 is
`0f1f626b04d6ccf3a81b5a410a5cef771137978f599fbb3a76aa0fbe03728645`.

- `stage5-host.log`: resident static/glibc/inherited-filter/exec/VFS/pack/DNS
  suite passes, including group-main, group-worker, thread-main and group-race.
  Blocking and CPU-spinning siblings are included. Each host test runs twice
  and verifies resident fd/VMA/cwd/umask/rlimit/fault-action sentinels.
- `stage5-group-stress.log`: 100 host processes per lifecycle mode, two sessions
  each, 800 sessions total, pass after the late-wakeup fix. Before that fix,
  competing group exit intermittently killed the resident with SIGILL.
- `stage5-vulkan.log`: pinned Headers v1.4.341/hash checked; full forwarder suite
  passes, 26.5 ns single-thread, 26.5 ns with 64 live registrations, 29.3 ns
  CPU time with 64 concurrent threads, below the 40 ns budget.
- `stage5-proxy-suite.log`: full standalone proxy suite passes in a disposable
  source mirror. The first mirror lacked the sibling gpu-forward-spike headers;
  adding the worktree's own source headers and rerunning completed successfully.
- `stage5-apk-build.log`: repository tests, native/Java compilation, signing,
  16 KiB alignment and closure of 84 native dependencies pass through build.sh.
- `stage5-final-device-logcat.txt`: signed APK and generated stub on
  localhost:5556. Static/glibc/exec each run twice in main ART PID 16512, status
  zero, with a responsive main looper. Final lifecycle PIDs: clean 16582/status
  0, group-main 16631/status 37, group-worker 16683/status 37, thread-main
  16738/status 37. Fault PID 16790 dies; recreated PID 16822 logs task removal
  and terminates. ActivityManager records the process deaths.
- `stage5-final-device-checks.txt`: pidof returns no process four seconds after
  each of those five lifecycles. Corresponding `stage5-final-*-activities.txt`
  and `stage5-final-*-windows.txt` capture activity/window removal.

The diagnostic exercises ActivityManager/WindowManager's activity window and
ART's main looper. It does not start the real per-app compositor/audio/session
bus, create a Linux client Surface or attach an independent Binder death
observer. Those missing checks must not be inferred from process death alone.
There was a VM system-process restart around initial APK updates/normal task
replay; attribution is not established. Final evidence was collected afterward.
Normal task replay also hit the previously documented broker g_libintl_gettext
link failure. No broker/dependency change is included.

## Remaining implementation

The pinned Flatpak multicall is built as a Bionic executable. Calling its ELF
startup inside ART would repeat linker/TLS startup assumptions. Calling its
renamed main directly also does not isolate its libc environment, cwd, fd and
helper-launch effects. The existing public Flatpak planning adapter only
resolves packages/extensions; it does not produce Flatpak's run/bwrap mount
plan. No replacement of stock Flatpak decisions was introduced here.

Stage 4 still needs a callable setup adapter over the pinned Flatpak run path,
with explicit session environment/fd/path ownership and interception of the
final bwrap plan. Preserve that plan in the virtual mount table and enter the
final image through resident userspace exec, with no ART execve or native fork.
Helper subprocess assumptions must be removed deliberately. Then prove an
installed simple Flatpak under its generated stub UID on the VM.

Stage 5 still needs Linux-owned signal actions/delivery coordinated with ART
sigchain, production cleanup and death/status notification, real compositor
windows and independent Binder-death checks. The existing diagnostic alone is
not sufficient to switch production V3Launcher. Stages 6–7 remain separate:
every Linux child, including plain fork, requires another zygote-created
same-app process; plain fork uses eager identical-address state transfer.
Native fork of ART is forbidden.

## Reproduce

1. Use README's build environment with the external output above, private Ninja
   symlink directory and CMake path from the task. Run:
   `bash linux/compat-proxy/inprocess/test.sh "$MATON_SUBSYSTEM_OUT/stage5-host"`.
2. Run `./build.sh`, install the signed host APK, then reinstall the generated
   diagnostic stub to refresh its shared-library host path. Use
   `adb -s localhost:5556 install --no-incremental -r APK`.
3. Launch component
   `flatpak.org.matonos.InProcessTest/org.matonos.linux.stub.StubActivity`
   with `am start -W -S -f 0x18000000`. Pass string extra
   `org.matonos.linux.IN_PROCESS_LIFECYCLE_TEST` as `clean`, `group-main`,
   `group-worker`, `thread-main` or `fault`, and a fresh unique string extra
   `org.matonos.linux.IN_PROCESS_TEST_ID`. Check MatonInProcess logs, process
   death, empty pidof, and no remaining activity/window for that package.
4. The original boolean IN_PROCESS_SELF_TEST still runs resident return/exec
   checks and keeps ART alive. Use a different unique attempt id.
5. Repeat on Ryzen/RX 6600, Surface Pro 3 and HP ProDesk after an image/build
   update; only QEMU was used here. Real Flatpak and compositor acceptance must
   follow the missing implementation, not be substituted with these fixtures.

All edits and commits are confined to lsa-inproc/v3-in-process. No AOSP build,
kernel/policy change, shared-file change, push or merge was performed. No VM
files were adb-pushed. The host APK update and diagnostic stub remain installed;
the diagnostic has no live process/task/window after final verification.
