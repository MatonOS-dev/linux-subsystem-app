#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${MATON_HOST_SMOKE_WORK:-/mnt/data/aosp/out/pc-logs/host-smoke}
FLATPAK_USER_DIR=${MATON_HOST_SMOKE_FLATPAK_USER_DIR:-$WORK/flatpak-user}
FSCK=${FSCK_EROFS:-/mnt/data/aosp/out/pc-logs/erofs-pipeline/src/erofs-utils-1.9.4/fsck/fsck.erofs}
MKFS=${MKFS_EROFS:-$ROOT/linux/packs/prebuilt/x86_64/matonos-mkfs-erofs}
ANDROID_JAR=${MATON_ANDROID_JAR:-/mnt/data/aosp/prebuilts/sdk/current/public/android.jar}
JSON_JAR=${MATON_JSON_JAR:-/mnt/data/aosp/prebuilts/misc/common/json/json-prebuilt.jar}
APKSIG=$ROOT/linux/stubgen/prebuilt/apksig.jar
LOADER=$ROOT/linux/compat-proxy/build/linux-layer-v3
FLATPAK=$ROOT/linux/flatpak/prebuilt/static/x86_64/matonos-flatpak
BWRAP=$ROOT/linux/flatpak/prebuilt/static/x86_64/matonos-app-bwrap
HOST_LD=$(readlink -f /lib64/ld-linux-x86-64.so.2)
FILTER_SRC=$ROOT/tests/host-smoke/prehandler-filter.c
POLICY=$ROOT/linux/compat-proxy/build/app-policy.h
PACKER=$ROOT/tests/host-smoke/HostSmokePacker.java
DEFAULT_REFS=(net.sourceforge.ExtremeTuxRacer net.supertuxkart.SuperTuxKart org.gnome.TextEditor org.kde.kate md.obsidian.Obsidian com.valvesoftware.Steam)
if (($#)); then REFS=("$@"); else REFS=("${DEFAULT_REFS[@]}"); fi

for tool in flatpak ostree javac java cc timeout; do command -v "$tool" >/dev/null || { echo "missing host tool: $tool" >&2; exit 2; }; done
for file in "$FSCK" "$MKFS" "$LOADER" "$FLATPAK" "$BWRAP" "$ANDROID_JAR" "$JSON_JAR" "$APKSIG" "$POLICY"; do
  [[ -e $file ]] || { echo "missing required input: $file" >&2; exit 2; }
done
mkdir -p "$WORK"/{flatpak-user,classes,mnt,logs,images,staging}
WORK=$(readlink -m "$WORK")
export FLATPAK_USER_DIR
export XDG_DATA_HOME="$WORK/xdg-data" XDG_CONFIG_HOME="$WORK/xdg-config" XDG_CACHE_HOME="$WORK/xdg-cache"
mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"
mkdir -p "$WORK/host-tools"
cp "$BWRAP" "$WORK/host-tools/matonos-app-bwrap"
cp "$(command -v xdg-dbus-proxy)" "$WORK/host-tools/xdg-dbus-proxy"
REPORT="$WORK/host-smoke.md"
printf '# Host Flatpak smoke\n\nGenerated %s\n\n' "$(date -Is)" >"$REPORT"
printf 'Host: %s\nFlatpak: %s\n\n' "$(uname -a)" "$(flatpak --version)" >>"$REPORT"

if [[ ! -x $FSCK ]]; then echo "fsck.erofs is not executable: $FSCK" >&2; exit 2; fi
if ! flatpak remotes --user | awk '{print $1}' | grep -qx flathub; then
  flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
fi
is_installed() {
  local ref=$1
  flatpak list --user --columns=ref | grep -Fqx "${ref#app/}"
}

for ref in "${REFS[@]}"; do
  appref="app/$ref/x86_64/stable"
  log="$WORK/logs/$(echo "$ref" | tr '.:' '__').install.log"
  echo "Installing $appref"
  if flatpak install --user --noninteractive --or-update flathub "$appref" >"$log" 2>&1; then
    printf '## %s\n\n- Install: **OK** (`%s`)\n' "$appref" "$log" >>"$REPORT"
  else
    rc=$?; printf '## %s\n\n- Install: **FAIL (%s)** (`%s`)\n\n```text\n%s\n```\n\n' "$appref" "$rc" "$log" "$(tail -80 "$log")" >>"$REPORT"; continue
  fi
  if ! is_installed "$appref"; then
    echo "Flatpak did not register $appref" >>"$REPORT"; continue
  fi
done

mkdir -p "$WORK/classes"
javac -cp "$ANDROID_JAR:$JSON_JAR:$APKSIG" -d "$WORK/classes" \
  "$ROOT"/linux/stubgen/src/org/matonos/linuxhost/stubgen/*.java \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/DeployManifest.java" \
  "$ROOT/linux/subsystem/host/app/src/main/java/org/matonos/linux/stub/PackageMounts.java" \
  "$PACKER"
cc -O2 -Wall -Wextra -Werror "$FILTER_SRC" -o "$WORK/prehandler-filter"

for ref in "${REFS[@]}"; do
  appref="app/$ref/x86_64/stable"; is_installed "$appref" || continue
  packlog="$WORK/logs/$(echo "$ref" | tr '.:' '__').pack.log"
  if ! java -Xmx2g -cp "$WORK/classes:$ANDROID_JAR:$JSON_JAR:$APKSIG" \
      org.matonos.hostsmoke.HostSmokePacker "$FLATPAK_USER_DIR" "$WORK" "$MKFS" "$appref" >"$packlog" 2>&1; then
    printf '\n- Package images: **FAIL**\n```text\n%s\n```\n' "$(tail -80 "$packlog")" >>"$REPORT"; continue
  fi
  mounts=(); targets=(); images=(); backings=()
  while IFS=$'\t' read -r kind one two three four five; do
    [[ $kind == IMAGE ]] || continue
    image=$four; src=$five; leaf=$(echo "$one" | tr '/.' '__'); mount="$WORK/mnt/matonos/$(id -u)/$leaf"
    mkdir -p "$mount"; images+=("$one $two $three bytes"); mounts+=("$mount")
    if ! "$FSCK" --extract="$mount" "$image" >"$WORK/logs/extract-$leaf.log" 2>&1; then
      echo "EROFS extraction failed for $one: $(tail -20 "$WORK/logs/extract-$leaf.log")" >>"$REPORT"
    fi
    targets+=(--erofs-mount "$two" "$mount")
    backings+=(--erofs-backing "$two" "$src")
  done <"$packlog"
  # Use launch-time targets emitted by PackageMounts, retaining runtime branch suffixes.
  # The host has no mounted /apex. Preserve these two helper mappings in
  # bwrap's final virtual root too; device images get them from the APEX.
  launch_args=(--supervise --parent-pid "$$" --trace \
    --map-ro "$WORK/host-tools/matonos-app-bwrap" /apex/com.matonos.flatpak/bin/matonos-app-bwrap \
    --map-ro "$WORK/host-tools/xdg-dbus-proxy" /apex/com.matonos.flatpak/bin/xdg-dbus-proxy \
    --map-ro "$WORK/host-tools/matonos-app-bwrap" /tmp/newroot/apex/com.matonos.flatpak/bin/matonos-app-bwrap \
    --map-ro "$WORK/host-tools/xdg-dbus-proxy" /tmp/newroot/apex/com.matonos.flatpak/bin/xdg-dbus-proxy \
    --map-ro "$HOST_LD" /tmp/newroot/lib64/ld-linux-x86-64.so.2)
  while IFS=$'\t' read -r kind one two; do
    [[ $kind == TARGET ]] || continue
    source_mount=""
    while IFS=$'\t' read -r ikind image_ref split size img_path source; do
      [[ $ikind == IMAGE && $image_ref == "$one" ]] && { source_mount="$WORK/mnt/matonos/$(id -u)/$(echo "$image_ref" | tr '/.' '__')"; break; }
    done <"$packlog"
    [[ -n $source_mount ]] || continue
    if [[ $one == runtime/* ]]; then branch=${one##*/}; source_mount="$source_mount/$branch"; fi
    launch_args+=(--erofs-mount "$two" "$source_mount" --erofs-backing "$two" "$(awk -F'\t' -v r="$one" '$1=="IMAGE"&&$2==r{print $6;exit}' "$packlog")")
  done <"$packlog"
  display=; xvfbpid=
  if command -v Xvfb >/dev/null; then
    display=:$((100 + RANDOM % 400))
    Xvfb "$display" -screen 0 1280x800x24 -listen tcp -ac >"$WORK/logs/xvfb-$(echo "$ref" | tr '.:' '__').log" 2>&1 & xvfbpid=$!
    export DISPLAY="127.0.0.1$display"
    for _ in $(seq 1 50); do [[ -S "/tmp/.X11-unix/X${display#:}" ]] && break; sleep 0.1; done
  fi
  probes="$WORK/logs/$(echo "$ref" | tr '.:' '__').probes.log"
  : >"$probes"
  probe_prefix="$WORK/logs/$(echo "$ref" | tr '.:' '__').probe"
  for command in ldconfig; do
    if timeout 45 flatpak run --user --share=network --nosocket=x11 --command="$command" "$appref" -p >"$probe_prefix-$command.out" 2>"$probe_prefix-$command.err"; then
      printf 'probe %s: PASS\n' "$command" >>"$probes"
    else rc=$?; printf 'probe %s: exit %s; stderr=%s\n' "$command" "$rc" "$probe_prefix-$command.err" >>"$probes"; fi
  done
  appcmd=$(flatpak info --user --show-metadata "$ref" 2>/dev/null | awk -F= '/^command=/{print $2;exit}')
  if [[ -n $appcmd ]]; then
    if timeout 45 flatpak run --user --share=network --nosocket=x11 --command="$appcmd" "$appref" --version >"$probe_prefix-version.out" 2>"$probe_prefix-version.err"; then
      printf 'probe %s --version: PASS\n' "$appcmd" >>"$probes"
    else rc=$?; printf 'probe %s --version: exit %s\n' "$appcmd" "$rc" >>"$probes"; fi
  else printf 'probe app command metadata: SKIP\n' >>"$probes"; fi
  if [[ ${MATON_HOST_SMOKE_HEADLESS_ONLY:-0} != 1 ]]; then
    if [[ -n $xvfbpid ]]; then
      launchlog="$WORK/logs/$(echo "$ref" | tr '.:' '__').launch.log"
      set +e
      { printf 'argv:'; printf ' %q' "$WORK/prehandler-filter" "$LOADER" "${launch_args[@]}" \
          "$FLATPAK" --user run --no-documents-portal --no-session-bus --no-a11y-bus --share=network --nosocket=x11 --command="$appcmd" "$appref"; printf '\n'; } >"$launchlog"
      timeout --signal=TERM --kill-after=5 90 dbus-run-session -- \
        "$WORK/prehandler-filter" "$LOADER" "${launch_args[@]}" \
        "$FLATPAK" --user run --no-documents-portal --no-session-bus --no-a11y-bus --share=network --nosocket=x11 --command="$appcmd" "$appref" >>"$launchlog" 2>&1
      status=$?
      set -e
      kill "$xvfbpid" 2>/dev/null || true; wait "$xvfbpid" 2>/dev/null || true; xvfbpid=
      if ((status==0)); then launch_result='exit 0'; elif ((status==124)); then launch_result='timeout (process remained active)'; else launch_result="exit $status"; fi
      first=$(grep -m1 -E 'PREHANDLER_SIGSYS|SIGSYS|zygote|FATAL|error:|error while|file too short|Error:|Permission denied|No such file' "$launchlog" || true)
      blocked_syscall=$(grep -m1 'PREHANDLER_SIGSYS syscall=' "$launchlog" || true)
      [[ -n $blocked_syscall ]] || blocked_syscall=none
      printf '\n- Package images: **%s**\n- Mounted images: %s\n- Launch targets: %s\n- Probes:\n```text\n%s\n' \
        "$(grep -c '^IMAGE' "$packlog") per-package images" "${#mounts[@]}" "$(grep -c '^TARGET' "$packlog" || true)" "$(cat "$probes")" >>"$REPORT"
      for probe in ldconfig version; do
        [[ -f $probe_prefix-$probe.err ]] && { printf '\n%s stderr:\n' "$probe" >>"$REPORT"; cat "$probe_prefix-$probe.err" >>"$REPORT"; }
      done
      printf '```\n- Real launch under Android pre-handler filter: **%s**\n- First diagnostic: `%s`\n- First blocked syscall: `%s`\n- Launch log: `%s`\n' "$launch_result" "${first:-none}" "$blocked_syscall" "$launchlog" >>"$REPORT"
    else printf '\n- GUI launch: **SKIP** (Xvfb unavailable)\n- Probes:\n```text\n%s\n```\n' "$(cat "$probes")" >>"$REPORT"; fi
  else printf '\n- Package images: %s\n- Mounted images: %s\n- Launch targets: %s\n- GUI launch: **SKIP** (headless-only mode)\n- Probes:\n```text\n%s\n```\n' "$(grep -c '^IMAGE' "$packlog")" "${#mounts[@]}" "$(grep -c '^TARGET' "$packlog" || true)" "$(cat "$probes")" >>"$REPORT"; fi
  if [[ -n ${xvfbpid:-} ]]; then kill "$xvfbpid" 2>/dev/null || true; wait "$xvfbpid" 2>/dev/null || true; xvfbpid=; fi
done

printf '\n## Host/device gaps\n\n- The host uses the production userspace image writer, EROFS FUSE mounts, launch target planner, native loader/bwrap/Flatpak path, and generated app syscall allow-list as a pre-handler seccomp filter. `erofsfuse` exposes some runtime symlinks as regular files containing the link text (for example, the 17-byte `libcurl.so.4` entry), so affected apps stop before their device-specific blocker.\n- Host launches use Xvfb over loopback TCP and `--no-session-bus`: the host has no mounted `/apex` helper and the host-built D-Bus proxy cannot load the runtime library through the host proxy path. This skips the device session-bus proxy.\n- Android PackageInstaller, PackageManager split arrays/signature verification, init-driven `matonos-erofsd` mounts, Android SELinux, the real installed seccomp timing, graphics bridge, Wayland/session broker, and device GPU drivers are not present on the host.\n' >>"$REPORT"
echo "Report: $REPORT"
