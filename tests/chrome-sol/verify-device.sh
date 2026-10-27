#!/usr/bin/env bash
# Coordinator only: freshly booted image/APK containing chrome-sol. No VM starts.
set -euo pipefail
: "${ADB_SERIAL:?set the coordinator VM adb serial}"
: "${CHROME_TEST_OUT:?set an output directory outside source projects}"
adb_bin=${ADB:-/mnt/data/aosp/out/host/linux-x86/bin/adb}
adb=("$adb_bin" -s "$ADB_SERIAL")
mkdir -p "$CHROME_TEST_OUT"
"${adb[@]}" root
"${adb[@]}" wait-for-device
"${adb[@]}" shell setprop persist.vendor.maton.sleep_idle_s 0
package() { "${adb[@]}" shell pm list packages | tr -d '\r' | sed -n 's/^package:\(flatpak\.app\.com_google_Chrome\.x86_64\.stable\..*\)$/\1/p'; }
pkg=$(package)
if [[ ${TRACE:-0} == 1 && -n $pkg ]]; then
    "${adb[@]}" shell "mkdir -p /data/user/0/$pkg/cache; touch /data/user/0/$pkg/cache/v3-trace"
fi
# Start capture before the request; retain the full log so SELinux and proxy
# evidence can be correlated by PID and timestamp. No global logcat clearing.
"${adb[@]}" logcat -v threadtime -T 1 >"$CHROME_TEST_OUT/logcat.txt" &
log_pid=$!
trap 'kill "$log_pid" 2>/dev/null || true; wait "$log_pid" 2>/dev/null || true' EXIT
"${adb[@]}" shell am start -n org.matonos.linux/.DebugExtraInstallActivity \
    -a org.matonos.linux.DEBUG_INSTALL_CHROME
until grep -qE 'Chrome extra-data install (succeeded|failed)|Could not start Chrome install|Could not bind store service|Store service disconnected' "$CHROME_TEST_OUT/logcat.txt"; do sleep 1; done
if grep -qE 'Chrome extra-data install failed|Could not start Chrome install|Could not bind store service|Store service disconnected' "$CHROME_TEST_OUT/logcat.txt"; then
    grep -E 'Flatpak Spawn lookup|Prepared Flatpak app deployment|create-denied|create-dirfd|v3 create (input|backing)|Creating temp file|Race condition binding dirfd|v3 mount|v3 exec|execvp|fsck\.erofs rejected stub image|possible signal|exit status|avc: denied|Chrome extra-data install failed|Could not start Chrome install' "$CHROME_TEST_OUT/logcat.txt" || true
    echo "Install failed; return logcat.txt to chrome-sol. Do not call this a launch pass." >&2
    exit 1
fi
pkg=$(package)
[[ -n $pkg && $pkg != *$'\n'* ]]
"${adb[@]}" shell dumpsys package "$pkg" >"$CHROME_TEST_OUT/package.txt"
"${adb[@]}" shell pm path "$pkg" >"$CHROME_TEST_OUT/splits.txt"
# Chrome's stock wrapper reads XDG_CONFIG_HOME/chrome-flags.conf inside
# Flatpak. Its backing is HOME/.var/app/com.google.Chrome/config/.
app_data=/data/user/0/$pkg
flags_dir=$app_data/home/.var/app/com.google.Chrome/config
"${adb[@]}" shell "mkdir -p '$flags_dir'; printf '%s\n' --ozone-platform=wayland >'$flags_dir/chrome-flags.conf'; uid=\$(stat -c %u '$app_data'); chown -R \$uid:\$uid '$app_data/home'"
"${adb[@]}" shell "test ! -e '$app_data/install'"
# Resolve the installed launcher instead of guessing its generated activity name.
component=$("${adb[@]}" shell cmd package resolve-activity --brief \
    -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -p "$pkg" | tr -d '\r' | tail -n 1)
[[ $component == "$pkg/"* ]]
"${adb[@]}" shell am start -n "$component"
sleep 8
{
    echo '=== Published private Flatpak deployments ==='
    "${adb[@]}" shell "for root in /data/user/0/$pkg/no_backup/work/flatpak-launch /data/user/0/$pkg/no_backup/work/flatpak-launch/system; do echo ROOT=\$root; ref=\$root/app/com.google.Chrome/x86_64/stable; readlink \$ref/active; ls -ld \$ref/34a83e470564192554ad103904cc56564c876f0032522199c6d3838e091cbf9e \$ref/34a83e470564192554ad103904cc56564c876f0032522199c6d3838e091cbf9e/files; cat \$root/repo/refs/heads/deploy/app/com.google.Chrome/x86_64/stable; done" 2>&1 || true
    echo '=== Running Chrome Flatpak identity ==='
    "${adb[@]}" shell 'for p in /proc/[0-9]*; do cmd=$(tr "\000" " " <"$p/cmdline" 2>/dev/null) || continue; case "$cmd" in *"/app/bin/chrome"*|*"/app/extra/chrome"*) echo "PID=${p##*/} CMD=$cmd"; cat "$p/root/.flatpak-info" 2>&1 || true;; esac; done' 2>&1 || true
} >"$CHROME_TEST_OUT/spawn-deployment.txt"
echo "Install, split publication and install/ cleanup passed. Launcher start requested; browser rendering still needs verification."
echo "Coordinator: verify a rendered Wayland window, browse a page, close and relaunch."
echo "Capture a screenshot and /proc/<Chrome PID>/cmdline to confirm --ozone-platform=wayland."
echo "Keep capture running for those checks; press Enter when finished."
read -r
"${adb[@]}" exec-out screencap -p >"$CHROME_TEST_OUT/screen.png"
