# Device trap-cost capture (coordinator; no new image or VM)

The agent did not run these commands. Use an existing **disposable installed
stub**, with no active session/data to preserve, on the coordinator's existing
device. Set `SERIAL` and `PKG` explicitly. This uses the existing `v3-command`
override in `V3Launcher.java`; it changes app data only. It does not replace
system files or the installed proxy. Thus the first capture measures the
**currently installed** proxy, not this branch's routing until it is shipped.

From this worktree, compile the two static Linux x86_64 payloads:

```bash
SERIAL=YOUR_ADB_SERIAL
PKG=YOUR_DISPOSABLE_STUB_PACKAGE
CAPTURE=/mnt/data/aosp/out/pc-logs/io-perf-device
mkdir -p "$CAPTURE"
cc -static -O2 -Wall -Wextra -Werror linux/compat-proxy/tests/io-trap-cost.c -o "$CAPTURE/trap-cost"
cc -static -O2 -Wall -Wextra -Werror linux/compat-proxy/tests/sigsys-cost.c -o "$CAPTURE/sigsys-cost"
ADB=/mnt/data/aosp/out/host/linux-x86/bin/adb
"$ADB" -s "$SERIAL" root
"$ADB" -s "$SERIAL" wait-for-device
"$ADB" -s "$SERIAL" shell mkdir -p /data/local/tmp/io-perf
"$ADB" -s "$SERIAL" push "$CAPTURE/trap-cost" "$CAPTURE/sigsys-cost" /data/local/tmp/io-perf/
"$ADB" -s "$SERIAL" shell 'chmod 755 /data/local/tmp/io-perf/trap-cost /data/local/tmp/io-perf/sigsys-cost; /data/local/tmp/io-perf/trap-cost 30000 /data/local/tmp/io-perf/native.txt /data/local/tmp/io-perf; /data/local/tmp/io-perf/sigsys-cost 30000 > /data/local/tmp/io-perf/signal.txt'
"$ADB" -s "$SERIAL" pull /data/local/tmp/io-perf/native.txt "$CAPTURE/native.txt"
"$ADB" -s "$SERIAL" pull /data/local/tmp/io-perf/signal.txt "$CAPTURE/signal.txt"
```

The shell-native rows are a kernel/hardware reference, **not** a matched stub
UID/SELinux/seccomp baseline. The `sigsys-cost` result is the native kernel
signal entry/return cost with a tiny SA_SIGINFO handler, no raw syscall in
that handler. It includes the filter and minimal handler, and must not be
called a pure kernel entry-only measurement. A stub-native comparison needs
an APK launching the same static binary without the proxy; none is added here.

Stage the payload and preserve existing override/profiling files. Run once:

```bash
"$ADB" -s "$SERIAL" shell sh -s -- "$PKG" <<'DEVICE'
set -eu
pkg=$1
case "$pkg" in ''|*[!A-Za-z0-9._]*) exit 2;; esac
data=/data/user/0/$pkg
uid=$(stat -c %u "$data")
save=$data/cache/io-perf-saved
# Fail rather than overwrite a previous capture's backups.
mkdir "$save"
for f in v3-command v3-trace v3-count v3-counts.txt; do
    if [ -e "$data/cache/$f" ]; then mv "$data/cache/$f" "$save/$f"; fi
done
mkdir -p "$data/home"
cp /data/local/tmp/io-perf/trap-cost "$data/home/io-trap-cost"
chmod 755 "$data/home/io-trap-cost"
chown "$uid:$uid" "$data/home/io-trap-cost"
restorecon "$data/home/io-trap-cost"
# Use the canonical /data/data name that the sandbox HOME uses.
printf '%s\n' "/data/data/$pkg/home/io-trap-cost" > "$data/cache/v3-command"
chown "$uid:$uid" "$data/cache/v3-command"
restorecon "$data/cache/v3-command"
DEVICE
```

Launch **without tracing or counters**, wait until the result ends in
`complete`, then pull it. No root process executes the proxied measurement;
the launcher runs it as the actual stub through its full Flatpak/proxy path.

```bash
"$ADB" -s "$SERIAL" shell am force-stop "$PKG"
COMPONENT=$("$ADB" -s "$SERIAL" shell cmd package resolve-activity --brief -a android.intent.action.MAIN -c android.intent.category.LAUNCHER "$PKG" | tr -d '\r' | tail -n 1)
"$ADB" -s "$SERIAL" shell am start -W -n "$COMPONENT"
# Repeat this read until it prints complete (normally a few seconds;
# the older 100-300 us/trap behavior may take a minute or longer).
"$ADB" -s "$SERIAL" shell tail -n 1 "/data/user/0/$PKG/home/io-trap-cost.txt"
"$ADB" -s "$SERIAL" pull "/data/user/0/$PKG/home/io-trap-cost.txt" "$CAPTURE/proxy.txt"
```

Now repeat with **counters only**, to compare OP-inclusive handler times with
end-to-end latency from the first run. Remove the previous result first to
avoid confusing an old `complete` marker with the new launch. Preserve this
capture separately; do not use counted timing as the headline latency.

```bash
"$ADB" -s "$SERIAL" shell am force-stop "$PKG"
"$ADB" -s "$SERIAL" shell "rm -f /data/user/0/$PKG/home/io-trap-cost.txt; touch /data/user/0/$PKG/cache/v3-count; chown \$(stat -c %u /data/user/0/$PKG):\$(stat -c %u /data/user/0/$PKG) /data/user/0/$PKG/cache/v3-count; restorecon /data/user/0/$PKG/cache/v3-count"
"$ADB" -s "$SERIAL" shell am start -W -n "$COMPONENT"
# Wait for complete as above, then for the loader to finish its exit dump.
"$ADB" -s "$SERIAL" shell tail -n 1 "/data/user/0/$PKG/home/io-trap-cost.txt"
"$ADB" -s "$SERIAL" pull "/data/user/0/$PKG/home/io-trap-cost.txt" "$CAPTURE/proxy-counted.txt"
"$ADB" -s "$SERIAL" pull "/data/user/0/$PKG/cache/v3-counts.txt" "$CAPTURE/counts.txt"
python3 linux/compat-proxy/tests/trap-report.py "$CAPTURE/native.txt" "$CAPTURE/proxy.txt" --signal "$CAPTURE/signal.txt" --counts "$CAPTURE/counts.txt" > "$CAPTURE/report.json"
cat "$CAPTURE/report.json"
```

Optional: repeat with `v3-trace` present **as well as** `v3-count`, saving
results/counts under different names. The difference isolates logging mode's
cost on this workload (not an additive phase breakdown). This produces many
logcat records; 30,000 loops is intentionally modest. Trace mode deliberately
traps ordinary reads/writes and does not measure native I/O routing.

Restore the previous app configuration and remove the two small payloads:

```bash
"$ADB" -s "$SERIAL" shell am force-stop "$PKG"
"$ADB" -s "$SERIAL" shell sh -s -- "$PKG" <<'DEVICE'
set -eu
data=/data/user/0/$1
save=$data/cache/io-perf-saved
test -d "$save"
for f in v3-command v3-trace v3-count v3-counts.txt; do
    rm -f "$data/cache/$f"
    if [ -e "$save/$f" ]; then mv "$save/$f" "$data/cache/$f"; fi
done
rmdir "$save"
rm -f "$data/home/io-trap-cost" "$data/home/io-trap-cost.txt"
rm -rf /data/local/tmp/io-perf
DEVICE
```

## Reading the evidence

- `ioctl-emulated`: FICLONE is answered EOPNOTSUPP by the proxy with no
  payload-operation raw syscall. End-to-end time approximates the proxy's
  fixed trap cost plus dispatch/guard cost. Native returns EBADF or ENOTTY;
  this row is a diagnostic, not a semantically matched operation speedup.
- `flock-trap-raw` / `fcntl-trap-raw`: tiny handler plus a gateway kernel call.
  Compare their native rows and the emulated-ioctl row. Includes different
  operation costs; do not present subtraction as a precise gateway phase.
- `getpid-native` / `clock-syscall-native`: filter/gateway reference; clock
  row explicitly uses a syscall instead of glibc's vDSO clock.
- `read-zero` / `write-zero`: payload copy/storage-free fd routing reference.
- Read/write/file rows: actual sequential syscalls and positive/negative
  path operations. `openat-close` includes close in both variants.
- OP counter times omit kernel signal entry/return and handler tail. Their
  timing instrumentation itself issues two raw clock_gettime calls per trap.
  Compare **counted end-to-end** against counted OP to study those omitted
  portions; compare uncounted against counted to identify observer overhead.
- If kernel-only SIGSYS is already tens/hundreds of microseconds, investigate
  the guest kernel/CPU/virtualization path. If only the proxy rows are large,
  use the OP and raw-call counts to narrow dispatch/VFS/logging instead.
  Collect SELinux/audit evidence from the device before blaming either.

The agent cannot report a measured device fixed cost until these captures
are returned. Host results cannot establish that number or explain the
previous Steam capture by themselves.
