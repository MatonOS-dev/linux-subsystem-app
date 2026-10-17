# Reproducible Bionic Flatpak

The runtime install prefix is **`/usr`**, with `sysconfdir=/etc`,
`localstatedir=/var` and `libdir=lib`. These are logical paths in the loader's
filesystem view, never directories on the build machine. `/usr` is the normal
Linux payload prefix after bubblewrap pivots into an app's runtime. Before that
pivot the Bionic CLI sees the real device filesystem: `/system`, `/apex` and
`/data` retain their meanings, and optional `/usr` resources are absent. We do
not install host dependency files on the device or expose the staging tree.

`BUILD_DIR/prefix` remains the build-only dependency tree expected by the APK,
planner and broker recipes. `DESTDIR=BUILD_DIR/stage` and its `usr -> ../prefix`
symlink stage Meson, Autotools and CMake installs there. Libcap uses its
`FAKEROOT` equivalent; BoringSSL and DullPGP archives/headers are copied explicitly.
The pkg-config wrapper overrides `prefix` to the staged tree. Installed
pkg-config and libtool metadata is relocated for linking (including packages
that hard-code `/usr/lib` instead of `${prefix}/lib`); runtime defines remain
`/usr`.
Never reuse an older dependency build configured with a host install prefix.

## Runtime path audit

* `LoaderCommand` starts the Bionic CLI without `--root`, adds the DNS socket,
  and maps a generated resolver file over `/etc/resolv.conf` **and**
  `/system/etc/resolv.conf` (the device's `/etc` symlink is resolved first).
  The compat proxy's mount table is inherited through fork/exec; bubblewrap
  subsequently supplies the Linux root. No install-prefix map is involved.
* `InstallTransaction` sets `FLATPAK_SYSTEM_DIR`, `FLATPAK_USER_DIR`,
  `FLATPAK_SYSTEM_CACHE_DIR` and `FLATPAK_DOWNLOAD_TMPDIR` to private directories.
  It sets HOME, TMPDIR and all XDG directories. TLS uses Android Conscrypt's
  `/apex/com.android.conscrypt/cacerts`, both in curl's configuration and in
  `SSL_CERT_DIR`, `CURL_CA_BUNDLE`, `G_TLS_CA_PATH`.
* `V3Launcher` supplies the private deployment's system/user installation paths,
  cache/temp/XDG paths, per-app session bus, Wayland and PulseAudio sockets.
  Flatpak's built-in `FLATPAK_CONFIGDIR` (`/etc/flatpak`) and `FLATPAK_DATADIR`
  (`/usr/share/flatpak`) are optional global installation/configuration/default
  remote lookups; neither caller currently sets `FLATPAK_CONFIG_DIR` or
  `FLATPAK_DATA_DIR`. Remote descriptors are supplied explicitly by the installer
  from assets or the selected origin. No configuration lived at the old host
  prefix, so no runtime migration or new maps are needed.
* OSTree actually probes `DATADIR/ostree/trusted.gpg.d` unless `OSTREE_GPG_HOME`
  overrides it (`ostree-gpg-verifier.c`). A missing global directory is allowed;
  Flathub verification uses the remote-specific keyring imported from the
  `.flatpakrepo` descriptor into the private installation. The new optional
  fallback is `/usr/share/ostree/trusted.gpg.d`, never a developer directory.
* GLib probes `/usr/lib/gio/modules` (overridable with `GIO_MODULE_DIR`), while
  its built-in GIO implementations are linked statically. No external modules
  are shipped. Gettext's `/usr/share/locale` is optional; translations were not
  shipped at the former host prefix either. The install process uses `LC_ALL=C`.
* GLib's D-Bus machine-ID lookup tries `/var/lib/dbus/machine-id`, then the
  hard-coded `/etc/machine-id`. The latter fallback is unchanged. Flatpak's
  sandbox setup separately checks these same two standard paths. No machine ID
  was obtained from `BUILD/prefix/var/lib/dbus/machine-id`.
* libxml2's optional catalog is `/etc/xml/catalog` (`XML_CATALOG_FILES` can
  override it). AppStream parsing does not require a host catalog. GLib's
  desktop-launch helper is `/usr/libexec/gio-launch-desktop`, followed by a
  PATH search (or `GIO_LAUNCH_DESKTOP`). Flatpak runs payloads using bubblewrap,
  not that desktop-launch helper. It was not shipped at the former path.
* Flatpak's bubblewrap path stays `/apex/com.matonos.flatpak/bin/matonos-app-bwrap`;
  the APK helper/multicall and proxy integration retain their existing handling.
  The intentionally absent xdg-dbus-proxy setting is unchanged: the stub-hosted
  bus provides filtering. Changing the prefix does not change these settings.

## Deterministic inputs

All target C/C++ compilations inherit `-ffile-prefix-map` and
`-fdebug-prefix-map` for the source cache, build directory, recipe and NDK.
CMake receives these flags explicitly because the NDK toolchain resets the
environment defaults. Meson passes relative source names, so the relative cache path from a component
build directory is mapped too. This removes `__FILE__`/`__FILE_NAME__` and debug
path dependence, including BoringSSL diagnostics. Libcap retains its own extra
compiler flags. The multicall dispatcher has no source-path diagnostics.

`SOURCE_DATE_EPOCH` defaults to `1767225600` (2026-01-01 UTC); use the same value
for both builds. `LC_ALL=C`, `TZ=UTC`, `ZERO_AR_DATE=1`, `ARFLAGS=crD` and LLVM's
deterministic archive default avoid locale, timestamps and archive metadata
variation. Archive repacking uses sorted shell glob order. The final
Meson/multicall link does not request a GNU build ID; the ELF retains the NDK's
Android identification note. No path-derived build ID is added. The recipe
rejects an output containing `/home/` or `/mnt/` strings.

## Repeat the comparison

Use the pinned NDK r30 and locked sources with the same host tools. Copy a
verified download cache to two distinct directories, then run `fetch.sh` and
`build.sh` once per directory. `fetch.sh` regenerates the source trees from pins.
For each invocation set separate absolute `CACHE_DIR`, `BUILD_DIR`, `OUT_DIR`,
`ANDROID_NDK`, and `JOBS=4`. Put CMake and a private directory containing only
the desired Ninja binary on PATH (not AOSP's entire build-tools directory).
For example, with `REPRO_ROOT` outside the repository:

```sh
for name in a b; do
  export CACHE_DIR="$REPRO_ROOT/cache-$name"
  export BUILD_DIR="$REPRO_ROOT/build-$name"
  export OUT_DIR="$REPRO_ROOT/out-$name"
  sh linux/flatpak/build-support/fetch.sh
  sh linux/flatpak/build-support/build.sh
done
sha256sum "$REPRO_ROOT"/out-*/matonos-flatpak
cmp "$REPRO_ROOT/out-a/matonos-flatpak" "$REPRO_ROOT/out-b/matonos-flatpak"
! strings "$REPRO_ROOT/out-a/matonos-flatpak" | grep -E '/home/|/mnt/'
```

Then use the repository README's signed APK build with `MATON_FLATPAK_CACHE`
and `MATON_FLATPAK_BUILD` pointing to one verified build. This comparison covers
the stripped multicall ELF; it does not claim that all dependency metadata,
intermediate objects or the signed APK are byte-identical across directories.

## Verified result (2026-10-17)

Two builds with separate source caches (`cache-a`, `cache-b`), build directories
and output directories using NDK r30/API 36 and `JOBS=4` produced identical ELFs:

```
5f6465068c6afeacac721e3dedda9b490d8d8bfb3239b1b915b3624c28f63469
```

`cmp` passed, and `strings | grep -E '/home/|/mnt/'` returned no matches for
either output. The complete root `./build.sh` passed the host suites, native
builds, release APK packaging, signing verification and 16 KiB ZIP alignment.
The signed APK's stored `lib/x86_64/libmatonos-flatpak.so` exactly matches the
verified ELF. Shared-library requirements remain libc, libm and libdl. No VM
or hardware execution is claimed by this build reproducibility check.
