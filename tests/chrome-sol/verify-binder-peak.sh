#!/usr/bin/env bash
# Coordinator only: run against a freshly booted image with the updated host APK.
set -euo pipefail
serial=${1:?adb serial required}
out=${2:?external output directory required}
adb_bin=${ADB:-/mnt/data/aosp/out/host/linux-x86/bin/adb}
mkdir -p "$out"
a=("$adb_bin" -s "$serial")
"${a[@]}" shell cmd package list packages -U > "$out/packages.txt"
"${a[@]}" logcat -v threadtime > "$out/logcat.txt" &
log_pid=$!
trap 'kill "$log_pid" 2>/dev/null || true; wait "$log_pid" 2>/dev/null || true' EXIT
for app in org_kde_kate com_google_Chrome; do
    pkg=$("${a[@]}" shell pm list packages | tr -d '\r' | sed -n "s/^package:\(flatpak\.app\.${app}\..*\)/\1/p")
    [[ -n $pkg && $pkg != *$'\n'* ]] || { echo "Expected one installed $app package" >&2; exit 1; }
    "${a[@]}" shell am force-stop "$pkg"
    "${a[@]}" shell run-as "$pkg" rm -f cache/v3-trace
    "${a[@]}" shell am start -n "$pkg/.Launcher" > "$out/$app-start.txt"
    for ((elapsed=0; elapsed<=140; elapsed++)); do
        if ((elapsed<=20 || elapsed%5==0)); then
            { date -Is; printf 'elapsed=%s package=%s\n' "$elapsed" "$pkg"
              "${a[@]}" shell pidof "$pkg" || true
              "${a[@]}" shell dumpsys activity binder-proxies
            } >> "$out/$app-counts.txt"
        fi
        sleep 1
    done
    "${a[@]}" shell am force-stop "$pkg"
done
