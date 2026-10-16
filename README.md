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
APIs are app-owned; no system bridge compile snapshot is needed.

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

Xwayland, xkbcomp, X11 libraries, GL forwarders and keyboard data are packaged
in the APK. `linux/subsystem/build-xwayland.sh` and `UPSTREAMS` own the pinned
build recipe/provenance; `tools/package-xwayland.py` packages that output.
Executables are `libmatonos-Xwayland*.so` and `libmatonos-xkbcomp.so` native
libraries. Keyboard data is extracted from assets into each stub's private
runtime directory, keyed by host update time. Private symlinks retain the
library SONAMEs and the stock xkbcomp name. A small C entry point passes the
keyboard directory to the server and sets its working directory for xkbcomp.
No executable is extracted to writable app data. The image extracts native
libraries from the same verified APK into its app-local nativeLibraryDir;
PackageManager extracts installed updates. Global image Xwayland files are
no longer used. The server is built with `-Dxkb_bin_dir=.`; a stale dependency
build must be refreshed before packaging. Select the inherited Bionic crypto
prefix with `MATON_XWAYLAND_CRYPTO_PREFIX`; its pins and retained-version
exception are in `UPSTREAMS` (the static musl Flatpak prefix cannot replace it).

Device checks after a fresh image boot: confirm the subsystem uses `priv_app`,
execute its own APK native helpers (Flatpak and both Xwayland entry points),
install/download using network and PackageInstaller, then launch an X11 app,
type several keyboard layouts and close/reopen it. Verify Xwayland and xkbcomp
come from the host nativeLibraryDir, no `/system_ext/bin/Xwayland` exists and
no SELinux denials occur. Install a same-signer host update and relaunch an
existing stub to check that its binaries and keyboard assets follow the update.

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
