# MatonOS Flatpak stub APK format

A stub is a signed package record for one installed Flatpak ref. It contains
no application implementation. The shared code is supplied by the updatable
`org.matonos.linux` host APK through Android's dynamic shared-library
mechanism. The per-device signing key is held by Android Keystore.

## Package contents

The base APK contains the host shared-library declaration, `StubActivity`,
install/update activities, `StubService`, Flatpak identity metadata and the
extra-data install service when needed. The base has no launcher filter. A code-free
`launcher_<sha256>` split carries the launcher alias, label, MIME filters and
icon. The `index_<sha256>` split carries object indexes and OSTree metadata;
content uses `pack_<sha256>` carrier splits. One signed EROFS image split
contains the complete deployed tree for that package, including stock
Flatpak's `/app/extra` output when the ref declares extra data.

## Install and extra-data flow

The subsystem app first pulls and authenticates the commit with
`flatpak install --no-deploy`. For refs with `[Extra Data]`, it installs a
small signed stub with no launcher entry or app icon. That stub runs stock
`flatpak install` under the package UID, including Flatpak's own bwrap
`apply_extra` path, then builds the single EROFS image with the same mkfs
options as the subsystem. It returns the image through a file descriptor.

The subsystem extracts the image with the pinned `fsck.erofs` and compares
every path outside `/app/extra` against the authenticated OSTree commit
(file checksum, mode and symlink target). Under `/app/extra` it accepts only
regular files, directories and non-escaping relative symlinks without
setuid/setgid bits. It then uses that validated image in the normal EROFS
split and reads launcher metadata from the extracted deployment, including
extra-data exports. Refs without extra data keep the existing in-subsystem
deploy path.

## Host interface and tests

`HostContract.INTERFACE_VERSION` is the host ABI version. `StubService` hosts
the per-app runtime; the subsystem app owns install and update actions.

`linux/stubgen` generates signed stubs and code-free launcher/index/image
splits. Its host harness uses AOSP's `aapt2` to parse generated manifests.
The end-to-end extra-data test is an ordinary store install of Flathub's
`com.google.Chrome`. On a fresh userdebug image, set
`persist.vendor.maton.sleep_idle_s` to `0`, install Chrome from the store,
confirm the install finishes, inspect the package's one EROFS image for
`/app/extra`, launch it and confirm its extra-data resources load. Repeat
after uninstall/reinstall to cover cleanup. Host validator tests cover a good
image and reject a changed committed file, setuid content and escaping
symlink.

The package install itself exercises the returned file descriptor and the
package-specific UID. No isolated-process probe remains.
