# MatonOS Linux subsystem app — v3.5

Standalone source repository for the signed `org.matonos.linux` host APK,
generated stubs, OSTree packs, the compatibility proxy/loaders, Flatpak and
embedded PipeWire. MatonOS is based on AOSP. The device tree imports a signed
prebuilt; it does not compile this repository.

## Build and tests

Install JDK 21+, Gradle 9.4.1, SDK platform 36/build-tools 36.0.0 and NDK
30.0.16248370 (r30). Native dependency builds also require a C/C++ host toolchain,
Meson, Ninja, CMake, pkg-config, autotools, Python 3.12+, curl, tar, unzip,
OpenSSL, and the development packages for GLib and OSTree. The tests execute
host `ostree`, compile C/Java fixtures and use the SDK's `aapt2`. No AOSP or
device-tree checkout is required: build-only platform headers, their revision
pins and licences are in `third_party/platform-headers`; the bridge AIDL
compile contract is carried in `systembridge/aidl`.

```sh
export JAVA_HOME=/path/to/jdk-21
export MATON_ANDROID_SDK=/path/to/android-sdk
export MATON_NDK=/path/to/android-ndk-r30
export MATON_GRADLE_HOME=/path/to/gradle-9.4.1
export MATON_SUBSYSTEM_OUT=/absolute/path/out/linux-subsystem-app
export MATON_SIGNING_STORE_FILE=/private/path/subsystem.jks
export MATON_SIGNING_PASSWORD_FILE=/private/path/subsystem.pass
export MATON_SIGNING_KEY_ALIAS=matonos-dev
./build.sh --build-dependencies  # prepare locked dependencies, test, build, sign
./build.sh                       # reuse prepared dependencies; test, build, sign
```

The script requires an existing external keystore; it never creates or stores
keys in this public repository. It rejects signing inputs and output directories
inside the repository. The password file contains the keystore/key password.
Do not use shell tracing with signing environment variables. Maximum parallelism
is four (`MATON_BUILD_JOBS=1..4`). The result is
`$MATON_SUBSYSTEM_OUT/MatonLinuxSubsystem.apk`; JNI/dex entries are stored,
16 KiB aligned and signed again after ZIP normalization. Updates retain the
same signer. The script verifies the final signature and alignment.

Flatpak's source/dependency lock is `linux/flatpak/build-support/sources.lock`:
MatonOS-dev/flatpak branch `matonos/v26.10`, commit
`4cce363ef3168baf10a4cd0a916694d3959f9558`. Its CLI/multicall recipe was
carried from MatonOS_apexs; image-only store helpers are excluded. The build
keeps source archives and dependency output outside this repository. An existing
verified dependency cache can be selected with `MATON_FLATPAK_CACHE`,
`MATON_FLATPAK_BUILD`, `MATON_COMPOSITOR_BUILD` and `MATON_PIPEWIRE_BUILD`.
The source pin is checked before the APK build. `UPSTREAMS` and the native
recipes retain the other version/hash pins; this extraction does not upgrade them.

`tests/run.sh OUTPUT` runs signed carrier/stub/planner tests, pack C/Java
round trips, host commands, Java portals and native bus interoperability.
`linux/compat-proxy/tests/run.sh` is the Linux-host loader regression suite;
run it in a disposable copy next to `packs/` with a writable `build/` directory,
all outside the source tree. Android instrumentation tests require a device.
`python3 tests/scan-history.py` checks all reachable history, including deleted
blobs, for signing/private-key filenames and private-key/JKS contents.

## Split boundaries and provenance

History was extracted using `git filter-repo` on a fresh, non-local clone of
the device repository's `ostree-packs` branch at `28ecbe6`. Original authors,
commit messages and relevant file history remain; commit IDs were rewritten.
No remote repository was created and nothing was pushed.

The native `linux/dbus-broker` is **still used on this source branch**:
`SessionBus` calls `runtime.NativeBroker`, which loads
`libmatonos-dbus-broker.so`. Java implements desktop portals and their wire
codec; it has not replaced that session bus. Removing the JNI broker would
break per-app bus startup, so it and its tests are retained. It now links the
same pinned Flatpak GLib dependency build, eliminating its former separate
AOSP libffi/PCRE build. PipeWire is also live (`NativeAudio` and
`libmaton_pipewire.so`); its embedded library and notices remain here.

Xwayland's server, xkbcomp and system runtime/data remain image components,
preserved in the device tree under `prebuilt/xwayland/`. This repository
builds only the X11 client libraries needed by wlroots during APK dependency
bootstrap. The source recipe for the server is retained for provenance.
System image policy, bridge implementation/allowlist and image packaging stay
in the device repository. The standalone AIDL directory is a compile snapshot;
refresh it deliberately when the image's bridge contract changes.

## Validation and limits

The v3.5 split was built with SDK/NDK paths and prepared dependency caches,
without using device-tree sources. The carried Flatpak recipe rebuilt the
pinned fork against cached dependency builds. Signed APK verification, 16 KiB
alignment, the host suites and device preflight were checked; see the device
integration report for exact results and hashes. A completely empty-cache
bootstrap, AOSP image build, fresh VM boot and real hardware runtime tests
are separate integration checks, not claimed by the extraction.

For runtime verification, build a fresh image containing the pinned APK, boot
it (disable idle suspend in a test VM), install/update a signed OSTree carrier,
launch Wayland and X11 apps, check audio and close/reopen behaviour, then install
a same-signer host update and relaunch a stub. Repeat on Ryzen/RX 6600,
Surface Pro 3 and HP ProDesk; inspect SELinux denials and retain the no-hardware
fallbacks. No kernel or policy changes are required by this repository split.

Licences are preserved at the root and under the individual components,
including `linux/flatpak/licenses`, build-support licences, apksig and platform
header notices. No signing keys belong here.
