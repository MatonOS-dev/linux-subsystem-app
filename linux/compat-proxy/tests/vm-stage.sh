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
    adb -s "$serial" shell "env -i PATH=/usr/bin:/bin HOME='$stage' TMPDIR='$stage/tmp' '$stage/linux-layer-v3' --android-filter --root '$root' $* >'$stage/$name.out' 2>'$stage/$name.log'; code=\$?; echo \$code >'$stage/$name.status'"
    adb -s "$serial" pull "$stage/$name.out" "$stage/$name.log" "$stage/$name.status" "$logs/"
    test "$(tr -d '\r\n' <"$logs/$name.status")" = 0
}
# root must expose /data -> /data, /proc -> /proc, /dev -> /dev as well as /usr.
run_stage legacy "'$stage/legacy' '$stage/legacy-fixture'"
run_stage paths "'$stage/paths' '$stage/fixture' '$stage/nested'"
run_stage bash "/bin/bash -c 'printf pipe-ok | /bin/cat; ls /; cat /etc/os-release'"
# Optional existing glibc bwrap inside the runtime.
if [[ -n ${COMPAT_BWRAP:-} ]]; then
    run_stage bwrap "'$COMPAT_BWRAP' --ro-bind / / --tmpfs /tmp --proc /proc --dev /dev /bin/true"
    run_stage bwrap-fixture "'$COMPAT_BWRAP' --ro-bind / / --tmpfs /tmp --bind '$stage/fixture' /tmp/fixture --proc /proc --dev /dev /bin/cat /tmp/fixture/fixture"
fi
if [[ ${COMPAT_STAGE2:-0} == 1 ]]; then
    # Android tools execute with an empty payload mount table. bwrap alone
    # establishes the runtime root; never give the bionic CLI --root "$root".
    # The coordinator supplies the Runtimes UID from this device's package list.
    runtime_uid=${COMPAT_RUNTIME_UID:?Set COMPAT_RUNTIME_UID to org.matonos.linuxruntimes UID}
    [[ $runtime_uid =~ ^[0-9]+$ ]] && (( runtime_uid >= 10000 && runtime_uid < 20000 ))
    test "$(adb -s "$serial" shell id -u | tr -d '\r\n')" = 0
    command="env -i PATH=/apex/com.matonos.flatpak/bin:/system/bin:/system/xbin HOME=/data/matonos/linux/flatpak-data XDG_DATA_HOME=/data/matonos/linux/flatpak-data/.local/share XDG_CONFIG_HOME=/data/matonos/linux/flatpak-data/.config XDG_CACHE_HOME=/data/matonos/linux/cache XDG_RUNTIME_DIR=/data/matonos/linux/run TMPDIR='$stage/tmp' FLATPAK_SYSTEM_CACHE_DIR=/data/matonos/linux/cache FLATPAK_DOWNLOAD_TMPDIR='$stage/tmp' FLATPAK_USER_DIR=/data/misc_de/0/matonos-linux/install/${COMPAT_APP_UID:-10115} FLATPAK_SYSTEM_DIR=/data/misc_de/0/matonos-linux/runtime/$runtime_uid FLATPAK_BWRAP=/apex/com.matonos.flatpak/bin/matonos-bwrap MATON_APP_HOME_DIR=/data/misc_ce/0/matonos-linux/home/${COMPAT_APP_UID:-10115} LD_LIBRARY_PATH=/apex/com.matonos.flatpak/lib64:/system/lib64 '$stage/linux-layer-v3' --android-filter /apex/com.matonos.flatpak/bin/matonos-flatpak -v --user run --command=bash net.sourceforge.ExtremeTuxRacer -c 'ls /app /usr; head -2 /etc/os-release'"
    adb -s "$serial" shell "$command >'$stage/stage2.out' 2>'$stage/stage2.log'; code=\$?; echo \$code >'$stage/stage2.status'"
    adb -s "$serial" pull "$stage/stage2.out" "$stage/stage2.log" "$stage/stage2.status" "$logs/"
    test "$(tr -d '\r\n' <"$logs/stage2.status")" = 0
fi
printf 'VM logs: %s\n'  "$logs"
