# Reproducible static musl Flatpak tools

The shipped Flatpak multicall and app launcher are static PIE executables built
with the Alpine 3.24.2 x86_64 musl toolchain. They have no `PT_INTERP` and do
not load Android's Bionic runtime. Their installed names remain
`libmatonos-flatpak.so` and `libmatonos-app-flatpak.so` for APK compatibility.
Bubblewrap is also a static musl PIE at `libmatonos-app-bwrap.so`.

## Build environment

`build-support/fetch.sh` verifies `sources.lock` into `CACHE_DIR`.
`build-support/build-musl.sh` enters the offline root at
`$HOME/matonos/build/musl-324/rootfs/x86_64` and runs the current
`build-support/build.sh` recipe there. `MUSL_SCRATCH` can select another Alpine
root/scratch pair; `CACHE_DIR`, `BUILD_DIR` and `OUT_DIR` must remain below that
scratch path because `alpine-enter.sh` exposes it as `/scratch`.

The logical runtime prefix is `/usr`, with `sysconfdir=/etc`,
`localstatedir=/var` and `libdir=lib`. These paths belong to the Linux payload
filesystem after bubblewrap pivots into a Flatpak runtime. `DESTDIR` stages
build-only installs, and pkg-config/libtool metadata is rewritten to point at
that staging prefix. Compiled programs retain `/usr` runtime paths. Curl uses
`/apex/com.android.conscrypt/cacerts` for the Android Conscrypt CA store.

`build.sh` preserves the locked Flatpak, OSTree, GLib, curl, BoringSSL,
gpgme-lite and static dependency set. It no longer force-includes
`bionic-fill.h`, links NDK libc++, imports NDK zlib headers/libraries, or uses
Android CMake toolchain files. Installer DNS uses the existing per-app musl
DNS forwarder configured by `LoaderCommand`; no Android resolver symbols are
linked into the Linux tools.

## Runtime paths

* `LoaderCommand` starts the static musl CLI without `--root`, enables
  per-process DNS interception and maps the generated resolver file over
  `/etc/resolv.conf` and `/system/etc/resolv.conf`. The loader's VFS and mount
  table are inherited through fork/exec; bubblewrap later supplies the Linux
  root.
* `InstallTransaction` sets private Flatpak installation/cache/download
  directories and the XDG environment. TLS uses Android Conscrypt's CA store
  through curl and the GLib TLS environment variables.
* `V3Launcher` supplies private deployment, cache/temp/XDG paths, the session
  bus, Wayland and PulseAudio sockets. Flatpak and OSTree remote descriptors
  are supplied explicitly; global config/keyring paths remain optional.
* Flatpak keeps its configured bubblewrap path at
  `/apex/com.matonos.flatpak/bin/matonos-app-bwrap`. The APK and proxy keep the
  existing executable names and multicall dispatch behavior.

## Deterministic inputs

All target C/C++ compilations use source/build/recipe/toolchain prefix maps.
`SOURCE_DATE_EPOCH`, `LC_ALL=C`, `TZ=UTC`, `ZERO_AR_DATE=1`, and deterministic
archive flags avoid timestamps, locale and archive-order variation. The
multicall link uses the locked input objects; GCC emits its deterministic GNU
build ID by default. The build rejects `/home/` and `/mnt/` strings in the
final multicall except two fixed GCC source paths embedded in Alpine's
prebuilt `libstdc++` diagnostics; those paths are toolchain data, not source,
build or output directory paths.

For a repeat build, use the same Alpine 3.24.2 root and locked source pins,
with separate `CACHE_DIR`, `BUILD_DIR` and `OUT_DIR` below the same
external scratch directory. Run `fetch.sh` and `build-musl.sh` for each copy,
then compare the multicall hashes and bytes. `SOURCE` records source revisions
and binary hashes; `HELPERS.sha256` records the app launcher recipe and bwrap
artifact hash.

The host tests validate static musl startup and Flatpak/bubblewrap command
behavior through the compat proxy. VM acceptance additionally checks the
Tux Racer launch through `ldconfig` and a small Flatpak install through the
test installer. Host reproducibility alone does not claim VM or hardware
success.
