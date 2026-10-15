# Flatpak Development HostCommand (round 22)

On MatonOS, the "host" requested by `flatpak-spawn --host` is the calling
app's own Linux environment. This service never asks linuxd, a privileged
helper or another app to execute anything.

The per-app native session broker already owns org.freedesktop.Flatpak.
HostCommandBackend serves its Development v1 object through the same Java
backend as desktop portals. The exact signature comes from the staged Flatpak
source's `data/org.freedesktop.Flatpak.xml`: `ayaaya{uh}a{ss}u` → `u`, signal
method `uub`, exited signal `uu`, read-only uint version 1. The session-helper
source also supplies the setsid/controlling-tty behavior. Unknown flags,
malformed byte strings, invalid FD indexes and foreign caller PIDs are rejected.
Replies precede exit signals, which are unicast to the original bus sender.

V3Launcher gives the proxy a private `host-command` socket path. On the first
successful resident exec where the completed virtual root contains
`/.flatpak-info`, it forks a server before entering the app. That server's
memory snapshot preserves the runtime/extension mounts at /usr, app mounts at
/app, cwd, DNS translation, inherited filters, UID and proxy configuration.
Real setns/flatpak-enter would not restore these memory-resident mounts.
Commands fork from this snapshot and use the existing resident ELF loader.
Nothing falls back to ProcessBuilder, kernel host exec or a reconstructed root.

Requests transfer FDs with SCM_RIGHTS. Every source is duplicated above all
requested destinations before remapping, so repeated sources and descriptor
cycles work. Unspecified stdio uses /dev/null. The child becomes a session
leader; a supplied stdio tty is made its controlling tty when permitted by the
kernel. Raw host PIDs are also the app-visible PIDs here (the proxy's namespace
setup is virtual). The signal carries the raw wait4 status, including signal
termination, rather than a Java exit code. CLEAR_ENV removes the captured app
launch environment; otherwise supplied variables override it. Byte-string
argv/cwd are preserved without a UTF-8 round trip. PATH search uses the child's
merged PATH, or /usr/bin:/bin if absent, within the captured root.

Each command has a resident subreaper worker. WATCH_BUS closes its channel when
the authenticated sender disconnects. A worker follows the app's lifetime;
runtime backend shutdown also stops the server explicitly. It reaps adopted
children and kills them during teardown, including jobs that call setsid or
outlive the initial command. This needs PR_SET_CHILD_SUBREAPER and readable
/proc/self/task/<tid>/children; if unavailable, spawning returns an error rather
than starting an untracked tree. No new permission grant is supplied. Servers
are bounded to 128 workers and requests to 1 MiB/4096 args/4096 env entries/16
mapped descriptors. Commands targetting the proxy's private descriptors fail.

The proxy remains a compatibility layer. The inherited app UID, kernel filter
and SELinux domain provide containment. This feature does not turn its virtual
mount table into a security boundary.

## Validation

From the worktree root:

```
bash linux/compat-proxy/build.sh
python3 linux/compat-proxy/tests/host-command-pair.py
bash linux/subsystem/host/test-host-command.sh /mnt/data/aosp/out/pc-logs/v3-hostcmd/java
bash linux/subsystem/host/test-portals.sh /mnt/data/aosp/out/pc-logs/v3-hostcmd/portal-tests
MATON_V3_ANDROID_OUT=/mnt/data/aosp/out/pc-logs/v3-hostcmd/android bash linux/compat-proxy/build-android.sh
```

The socketpair test uses the production snapshot timing inside a host bwrap
root, under the copied app syscall filter. Only transport acquisition differs:
a test-only inherited descriptor is held through bwrap until snapshot creation.
It checks /app and cwd, /usr executable loading, environment merge/clear,
SCM_RIGHTS, stdin/out/err and fd42 sharing a pty, controlling tty, actual UID,
spawn errors, signal/raw exit status, WATCH_BUS, runtime death, and a detached
session surviving its initial command but dying with the app. The test hook is
excluded from the production NDK build. It does not reproduce SELinux or an
installed app's runtime extensions.

`tests/host-command.py` also exercises the production Unix listener on an
unrestricted host; this runner rejects pathname Unix connect with EPERM, so that
check is blocked here. The Java harness round-trips the actual D-Bus wire codec
with fd capability indexes, replies and signals; it tests ownership, flags,
version and reply-before-exit. Existing portal loopback checks likewise skip
under this runner. No device/adb/image tests were run.

## Integration and device steps

The required broker route is documented in SHARED-CHANGES.md. Until applied,
Development calls remain rejected before reaching Java. org.freedesktop.portal.
Flatpak Spawn is a separate API with sandbox/permission/options semantics; it
is not implemented by this change and must not be aliased to HostCommand.

The coordinator should apply that routing change, package the rebuilt loader
and compositor Java together, and boot a fresh image. Then:

1. Launch Rio normally; its shell should remain open and accept commands.
2. In that shell inspect /usr, /app, /.flatpak-info, pwd, id, env and tty. Compare
   them with the app's normal sandbox, not with a device root shell. Verify
   /dev/tty opens and shell job control works.
3. Use flatpak-spawn --host with a simple /usr/bin/sh command, a chosen working
   directory and env overrides; test --clear-env and nonzero/signal exit codes.
4. Start a long command with WATCH_BUS and terminate its flatpak-spawn caller;
   check that its tree is gone. Repeat without WATCH_BUS: it should keep running
   until app/runtime teardown.
5. Launch a background setsid child; close the app and confirm that neither it
   nor command supervisors remain. Relaunch Rio and repeat.
6. Repeat with a second app, and check foreign caller PID signaling is denied.
   If kernel/SELinux denies the subreaper or proc children read, collect the error
   and denial; the launch must fail cleanly without a host-exec fallback.

Generic behavior applies on Ryzen/RX6600, Surface Pro 3 and Haswell ProDesk;
there are no machine paths or per-app exceptions. No kernel change is requested.
