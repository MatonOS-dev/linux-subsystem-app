#!/usr/bin/env bash
# Coordinator-only VM runner; never reboot, change policy, or touch deployments.
set -euo pipefail
cd "$(dirname "$0")/.."
serial=${ANDROID_SERIAL:-localhost:5555}
stage=/data/local/tmp/compat-proxy
logs=${1:-build/vm-logs}
mkdir -p "$logs"
adb -s "$serial" shell "mkdir -p '$stage/tmp' '$stage/fixture' '$stage/nested' '$stage/legacy-fixture'"
adb -s "$serial" push build/linux-layer-v3 build/legacy build/paths "$stage/"
printf 'mount-ok\n' >build/vm-fixture
adb -s "$serial" push build/vm-fixture "$stage/fixture/fixture"
adb -s "$serial" shell "chmod 755 '$stage/linux-layer-v3' '$stage/legacy' '$stage/paths'; ln -sf /virtual-v3/fixture '$stage/fixture/absolute'; ln -sf fixture '$stage/fixture/relative'"
# Host test ELFs use the host glibc interpreter. Supply --root pointing to the
# coordinator's existing freedesktop runtime symlink root; no deployment edits.
root=${COMPAT_RUNTIME_ROOT:?Set COMPAT_RUNTIME_ROOT to the existing runtime symlink root on the VM}
run_stage() {
    local name=$1
    shift
    adb -s "$serial" shell "TMPDIR='$stage/tmp' '$stage/linux-layer-v3' --android-filter --root '$root' $* >'$stage/$name.out' 2>'$stage/$name.log'; code=\$?; echo \$code >'$stage/$name.status'"
    adb -s "$serial" pull "$stage/$name.out" "$stage/$name.log" "$stage/$name.status" "$logs/"
    test "$(tr -d '\r\n' <"$logs/$name.status")" = 0
}
# root must expose /data -> /data, /proc -> /proc, /dev -> /dev as well as /usr.
run_stage legacy "'$stage/legacy' '$stage/legacy-fixture'"
run_stage paths "'$stage/paths' '$stage/fixture' '$stage/nested'"
run_stage bash "/bin/bash -c 'printf pipe-ok | /bin/cat; ls /; cat /etc/os-release'"
# Optional existing glibc bwrap inside the runtime. The staged MatonOS bwrap is
# bionic-linked; do not label this test native-bionic support (see README).
if [[ -n ${COMPAT_BWRAP:-} ]]; then
    run_stage bwrap "'$COMPAT_BWRAP' --ro-bind / / --tmpfs /tmp --proc /proc --dev /dev /bin/true"
    run_stage bwrap-fixture "'$COMPAT_BWRAP' --ro-bind / / --tmpfs /tmp --bind '$stage/fixture' /tmp/fixture --proc /proc --dev /dev /bin/cat /tmp/fixture/fixture"
fi
printf 'VM logs: %s\n' "$logs"
