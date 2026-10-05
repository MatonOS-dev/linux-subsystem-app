#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 2 ]]; then
  echo "usage: $0 SYSTEM_INSTALL USER_INSTALL" >&2
  exit 2
fi
WORKTREE=$(cd "$(dirname "$0")/../../.." && pwd)
STATIC=${MATONOS_FLATPAK_TEST_BINARY:-$WORKTREE/linux/flatpak/prebuilt/static/x86_64/matonos-flatpak}
LAUNCHER_PROBE=${MATONOS_FLATPAK_LAUNCHER_PROBE:-}
SYSTEM_INSTALL=$(realpath "$1")
USER_INSTALL=$(realpath "$2")
[[ -x $STATIC && -d $SYSTEM_INSTALL && -d $USER_INSTALL ]] || {
  echo "static Flatpak binary and both published installs are required" >&2
  exit 2
}
if [[ -n $LAUNCHER_PROBE && ! -x $LAUNCHER_PROBE ]]; then
  echo "static launcher probe is not executable: $LAUNCHER_PROBE" >&2
  exit 2
fi
command -v strace >/dev/null || { echo "strace is required for the exec audit" >&2; exit 2; }

TMP=$(mktemp -d "$WORKTREE/.flatpak-launch-test.XXXXXX")
listener_pid=
cleanup() {
  if [[ -n $listener_pid ]]; then kill -TERM "$listener_pid" 2>/dev/null || true; wait "$listener_pid" 2>/dev/null || true; fi
  rm -rf "$TMP"
}
trap cleanup EXIT
mkdir -p "$TMP/bin" "$TMP/udev"
printf '0123456789abcdef0123456789abcdef\n' > "$TMP/machine-id"
: > "$TMP/udev/control"
if [[ -n ${MATONOS_BWRAP_BINARY:-} ]]; then
  install -m755 "$MATONOS_BWRAP_BINARY" "$TMP/matonos-bwrap"
else
  cc -std=c11 -Wall -Wextra -Werror -static \
    -DMATON_FLATPAK_BIN="\"$TMP/bin\"" \
    -DMATON_MACHINE_ID_PATH="\"$TMP/machine-id\"" \
    -DMATON_UDEV_DB_PATH="\"$TMP/udev\"" \
    "$WORKTREE/linux/flatpak/matonos-bwrap.c" -o "$TMP/matonos-bwrap"
fi
cc -std=c11 -Wall -Wextra -Werror \
  "$WORKTREE/linux/flatpak/tests/fake-bus-listener.c" -o "$TMP/fake-bus-listener"
ln -s "$STATIC" "$TMP/bin/bwrap"
ln -s "$STATIC" "$TMP/bin/flatpak"
ln -s "$STATIC" "$TMP/bin/matonos-flatpak"
ln -s "$STATIC" "$TMP/matonos-flatpak"
"$TMP/fake-bus-listener" "$TMP/session-bus.sock" &
listener_pid=$!
for _ in {1..100}; do
  [[ -S $TMP/session-bus.sock ]] && break
  sleep 0.02
done
[[ -S $TMP/session-bus.sock ]] || { echo "fake broker socket did not start" >&2; exit 1; }

probe='test "$DBUS_SESSION_BUS_ADDRESS" = unix:path=/run/flatpak/bus
test -z "${AT_SPI_BUS_ADDRESS+x}"
test -S /run/flatpak/bus
test ! -e /dev/binder && test ! -e /dev/vndbinder && test ! -e /dev/hwbinder
test ! -w /app && test ! -w /usr
! touch /app/.matonos-write-probe 2>/dev/null
! touch /usr/.matonos-write-probe 2>/dev/null
printf "launch-chain-probe-ok\\n"'
run_probe() {
  env -u AT_SPI_BUS_ADDRESS \
    MATONOS_BWRAP_PROBE_ROOT="$TMP" \
    FLATPAK_SYSTEM_DIR="$SYSTEM_INSTALL" \
    FLATPAK_USER_DIR="$USER_INSTALL" \
    FLATPAK_BWRAP="$TMP/matonos-bwrap" \
    FLATPAK_DBUSPROXY="$TMP/should-not-start-xdg-dbus-proxy" \
    DBUS_SESSION_BUS_ADDRESS="unix:path=$TMP/session-bus.sock" \
    strace -f -e trace=execve -o "$TMP/exec.log" "$@" > "$TMP/app.log" 2>&1
}
if [[ -n $LAUNCHER_PROBE ]]; then
  run_probe "$LAUNCHER_PROBE" --host-probe "$STATIC" run --user --command=sh --no-a11y-bus \
    org.gnome.Calculator -c "$probe" || {
      cat "$TMP/app.log" >&2
      cat "$TMP/exec.log" >&2
      exit 1
    }
else
  run_probe "$TMP/bin/flatpak" run --user --command=sh --no-a11y-bus \
    org.gnome.Calculator -c "$probe" || {
      cat "$TMP/app.log" >&2
      cat "$TMP/exec.log" >&2
      exit 1
    }
fi
grep -F 'launch-chain-probe-ok' "$TMP/app.log" >/dev/null || {
  cat "$TMP/app.log" >&2
  echo "sandbox probe did not report success" >&2
  exit 1
}
if grep -F 'xdg-dbus-proxy' "$TMP/exec.log" >/dev/null; then
  cat "$TMP/exec.log" >&2
  echo "Flatpak attempted to execute a D-Bus proxy" >&2
  exit 1
fi
echo "PASS real static Flatpak launch: broker socket bind, no a11y address, no proxy exec, no binder nodes, read-only app/runtime${LAUNCHER_PROBE:+, static-musl launcher handoff}"
