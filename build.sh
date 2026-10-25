#!/usr/bin/env bash
# v3.5 standalone APK build. All private signing inputs are supplied externally.
set -Eeuo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
: "${MATON_SUBSYSTEM_OUT:?set an output directory outside the source repository}"
: "${MATON_SIGNING_STORE_FILE:?path to existing external signing keystore}"
: "${MATON_SIGNING_PASSWORD_FILE:?path to external password file}"
export MATON_SIGNING_KEY_ALIAS=${MATON_SIGNING_KEY_ALIAS:-matonos-dev}
export MATON_SIGNING_STORE_PASSWORD=$(cat "$MATON_SIGNING_PASSWORD_FILE")
export MATON_SIGNING_KEY_PASSWORD=$MATON_SIGNING_STORE_PASSWORD
export MATON_BUILD_JOBS=${MATON_BUILD_JOBS:-$(nproc)}
[[ $MATON_BUILD_JOBS =~ ^[1-9][0-9]*$ ]] || { echo "jobs must be a positive integer" >&2; exit 2; }
# Install JDK 21+ or set JAVA_HOME explicitly.
if [[ -n ${JAVA_HOME:-} ]]; then export PATH="$JAVA_HOME/bin:$PATH"; fi
export ANDROID_HOME=${MATON_ANDROID_SDK:-$HOME/Documents/matonos-android-sdk}
export ANDROID_SDK_ROOT=$ANDROID_HOME
export MATON_ANDROID_JAR=$ANDROID_HOME/platforms/android-36/android.jar
export MATON_AAPT2=$ANDROID_HOME/build-tools/36.0.0/aapt2
export MATON_BUILD_ROOT=$MATON_SUBSYSTEM_OUT/context
export MATON_AOSP_ROOT=${MATON_AOSP_ROOT:-$MATON_BUILD_ROOT}
export MATON_NDK=${MATON_NDK:-$HOME/Documents/android-ndk-r30}
export MATON_ANDROID_NDK=$MATON_NDK ANDROID_NDK=$MATON_NDK
export MATON_COMPOSITOR_BUILD=${MATON_COMPOSITOR_BUILD:-$MATON_SUBSYSTEM_OUT/deps/compositor}
export MATON_COMPOSITOR_JNI_LIBS=$MATON_COMPOSITOR_BUILD/jniLibs
export MATON_PIPEWIRE_BUILD=${MATON_PIPEWIRE_BUILD:-$MATON_SUBSYSTEM_OUT/deps/pipewire}
export MATON_V3_ANDROID_OUT=${MATON_V3_ANDROID_OUT:-$MATON_SUBSYSTEM_OUT/deps/linux-subsystem-app/loader}
OUTPUT=$MATON_SUBSYSTEM_OUT
mkdir -p "$OUTPUT"
case $(realpath "$OUTPUT") in "$ROOT"|"$ROOT"/*) echo "Output must be outside the source repository" >&2; exit 2;; esac
case $(realpath "$MATON_SIGNING_STORE_FILE") in "$ROOT"/*) echo 'Signing key must be outside this public repository' >&2; exit 2;; esac
case $(realpath "$MATON_SIGNING_PASSWORD_FILE") in "$ROOT"/*) echo "Signing password must be outside this public repository" >&2; exit 2;; esac
FLATPAK=$ROOT/linux/flatpak/build-support
VULKAN_DEPS=${MATON_VULKAN_DEPS:-$OUTPUT/vulkan-forward}
export MATON_VULKAN_OUT=${MATON_VULKAN_OUT:-$VULKAN_DEPS/forward}
export MATON_VULKAN_HEADERS=${MATON_VULKAN_HEADERS:-$VULKAN_DEPS/src/Vulkan-Headers-1.4.341}
export CMAKE=${CMAKE:-/mnt/data/aosp/prebuilts/cmake/linux-x86/bin/cmake}
if [[ ! -s $MATON_VULKAN_HEADERS/registry/vk.xml ]]; then
  bash "$ROOT/linux/vulkan-forward/build-tools.sh" "$VULKAN_DEPS"
fi
export MUSL_SCRATCH=${MATON_MUSL_SCRATCH:-/mnt/data/aosp/out/pc-logs/musl-324}
export FLATPAK_MUSL_WORK=${MATON_FLATPAK_SCRATCH:-$OUTPUT/flatpak/scratch/flatpak-musl}
# Source downloads are immutable during builds and remain shared. Every
# generated Flatpak file lives under this invocation's subsystem output.
export CACHE_DIR=${MATON_FLATPAK_CACHE:-$MUSL_SCRATCH/flatpak-musl/cache}
export BUILD_DIR=${MATON_FLATPAK_BUILD:-$FLATPAK_MUSL_WORK/build}
export OUT_DIR=${MATON_FLATPAK_OUT:-$FLATPAK_MUSL_WORK/out}
# Used by the separately Android-linked session broker build.
export MATON_FLATPAK_NDK=$OUTPUT/flatpak
export MATON_FLATPAK_PLAN_BUILD=${MATON_FLATPAK_PLAN_BUILD:-$BUILD_DIR/flatpak}
export MATON_FLATPAK_PLAN_OUT=${MATON_FLATPAK_PLAN_OUT:-$BUILD_DIR/plan}
# The Android-linked session broker still links the existing NDK dependency
# prefix; only Linux-side Flatpak executables use the musl prefix above.
export MATON_FLATPAK_PREFIX=$OUTPUT/flatpak/build/prefix
export MATON_FLATPAK_PLAN_OUT=$OUTPUT/plan
export MATON_FLATPAK_HELPER_WORK=$FLATPAK_MUSL_WORK/helper-build
export MATON_DBUS_ANDROID_OUT=$OUTPUT/broker
if [[ ${1:-} == --build-dependencies ]]; then
  JOBS=$MATON_BUILD_JOBS sh "$FLATPAK/fetch.sh"
  JOBS=$MATON_BUILD_JOBS bash "$FLATPAK/build-musl.sh"
  bash "$ROOT/tools/build-flatpak-plan-musl.sh"
  install -m755 "$OUT_DIR/matonos-flatpak" "$ROOT/linux/flatpak/prebuilt/static/x86_64/matonos-flatpak"
  python3 "$ROOT/tools/fetch-compositor.py"
  MATON_COMPOSITOR_BOOTSTRAP=1 bash "$ROOT/linux/subsystem/build-compositor.sh"
  bash "$ROOT/linux/subsystem/build-xwayland.sh"
  bash "$ROOT/linux/subsystem/build-compositor.sh"
fi
[[ -s $BUILD_DIR/flatpak/build.ninja ]] || { echo 'Build pinned dependencies first with --build-dependencies (see README)' >&2; exit 2; }
grep -q $'flatpak\t4cce363ef3168baf10a4cd0a916694d3959f9558\t' "$CACHE_DIR/src/flatpak.pin" || { echo 'Flatpak cache does not match the v3.5 source pin' >&2; exit 2; }
bash "$ROOT/tests/run.sh" "$OUTPUT/tests"
GRADLE=${MATON_GRADLE_HOME:-$HOME/Documents/gradle-9.4.1}/bin/gradle
REAL_VERSION_CODE=$(( $(git -C "$ROOT" rev-list --count HEAD) + 2 ))
[[ $REAL_VERSION_CODE -gt 1 ]] || { echo 'Real subsystem versionCode must be greater than bootstrap versionCode 1' >&2; exit 2; }
"$GRADLE" -p "$ROOT/linux/subsystem/host" --no-daemon --project-cache-dir "$OUTPUT/gradle-cache" --max-workers "$MATON_BUILD_JOBS" \
  -PrealVersionCode="$REAL_VERSION_CODE" :app:assembleRelease
SDK_TOOLS=$ANDROID_HOME/build-tools/36.0.0
python3 "$ROOT/tools/prepare-host-apk.py" "$OUTPUT/gradle/app/outputs/apk/release/app-release.apk" "$OUTPUT/stored.apk"
"$SDK_TOOLS/zipalign" -P 16 -f 4 "$OUTPUT/stored.apk" "$OUTPUT/aligned.apk"
"$SDK_TOOLS/apksigner" sign --ks "$MATON_SIGNING_STORE_FILE" --ks-key-alias "$MATON_SIGNING_KEY_ALIAS" --ks-pass "file:$MATON_SIGNING_PASSWORD_FILE" --out "$OUTPUT/MatonLinuxSubsystem.apk" "$OUTPUT/aligned.apk"
"$SDK_TOOLS/apksigner" verify --verbose --print-certs "$OUTPUT/MatonLinuxSubsystem.apk"
"$SDK_TOOLS/zipalign" -c -P 16 4 "$OUTPUT/MatonLinuxSubsystem.apk"
python3 "$ROOT/tools/check-apk-needed.py" "$OUTPUT/MatonLinuxSubsystem.apk" "$MATON_NDK"
echo "Signed APK: $OUTPUT/MatonLinuxSubsystem.apk"
"$GRADLE" -p "$ROOT/linux/subsystem/host" --no-daemon --project-cache-dir "$OUTPUT/gradle-cache" --max-workers "$MATON_BUILD_JOBS" \
  -PrealVersionCode="$REAL_VERSION_CODE" -PrealApk="$OUTPUT/MatonLinuxSubsystem.apk" :bootstrap:assembleRelease
"$SDK_TOOLS/zipalign" -P 16 -f 4 \
  "$OUTPUT/gradle/bootstrap/outputs/apk/release/bootstrap-release-unsigned.apk" "$OUTPUT/bootstrap-aligned.apk"
"$SDK_TOOLS/apksigner" sign --ks "$MATON_SIGNING_STORE_FILE" --ks-key-alias "$MATON_SIGNING_KEY_ALIAS" \
  --ks-pass "file:$MATON_SIGNING_PASSWORD_FILE" --out "$OUTPUT/MatonLinuxSubsystemBootstrap.apk" "$OUTPUT/bootstrap-aligned.apk"
"$SDK_TOOLS/apksigner" verify --verbose --print-certs "$OUTPUT/MatonLinuxSubsystemBootstrap.apk"
"$SDK_TOOLS/zipalign" -c -P 16 4 "$OUTPUT/MatonLinuxSubsystemBootstrap.apk"
python3 "$ROOT/tests/check-bootstrap-apks.py" "$OUTPUT/MatonLinuxSubsystemBootstrap.apk" "$OUTPUT/MatonLinuxSubsystem.apk" "$SDK_TOOLS/aapt2"
echo "Signed bootstrap APK: $OUTPUT/MatonLinuxSubsystemBootstrap.apk"
ISA_CHECKER=${MATON_ISA_CHECKER:-/mnt/data/aosp/device/maton/pc_x86_64/tools/check-isa.sh}
"$ISA_CHECKER" \
  --symbols-dir "$OUT_DIR" \
  --symbols-dir "$MATON_FLATPAK_PLAN_OUT" \
  --symbols-dir "$MATON_PIPEWIRE_BUILD/jniLibs/x86_64" \
  --symbols-dir "$MATON_COMPOSITOR_JNI_LIBS/x86_64" \
  --symbols-dir "$MATON_VULKAN_OUT" \
  "$OUTPUT/MatonLinuxSubsystem.apk" "$OUT_DIR/matonos-flatpak"
