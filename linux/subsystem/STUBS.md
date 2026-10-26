# MatonOS Flatpak stub APK format

A stub is a signed package record for one installed Flatpak ref. It contains
no application implementation. The shared code is supplied by the updatable
`org.matonos.linux` host APK through Android's dynamic shared-library
mechanism. The per-device signing key is held by Android Keystore.

## Package contents

The base APK contains the host shared-library declaration, `StubActivity`,
install/update activities, `StubService`, Flatpak identity metadata and an
empty valid `classes.dex`. The base has no launcher filter. A code-free
`launcher_<sha256>` split carries the launcher alias, label, MIME filters and
icon. The `index_<sha256>` split carries object indexes and OSTree metadata;
content uses `pack_<sha256>` carrier splits. One signed EROFS image split
contains the complete deployed tree for that package, including stock
Flatpak's `/app/extra` output when the ref declares extra data.

## Install and extra-data flow

The subsystem app's normal install process downloads refs with stock
`flatpak install --no-deploy`. Refs with an `[Extra Data]` section are then
deployed by stock `flatpak install --no-pull` in an
`android:isolatedProcess="true"` service. The app streams the local Flatpak
installation and a materialized OSTree repo as ustar over a pipe. The service
extracts the stream into bwrap's writable tmpfs and runs Flatpak's stock
deploy and `apply_extra` bwrap path there.
The isolated service has no app permissions, network permission or Keystore
access. It returns `/app/extra` as a ustar stream; the subsystem validates and
extracts regular files, directories and safe relative symlinks, then builds
the EROFS image itself. It never imports an image or filesystem ownership,
xattrs or timestamps from that process.

Refs without extra data keep the existing in-subsystem deploy. Extra-data
output is already present before launcher metadata is selected and before the
single image is built; there is no per-package extra service or extra split.

## Host interface and tests

`HostContract.INTERFACE_VERSION` is the host ABI version. `StubService` hosts
the per-app runtime; the subsystem app owns install and update actions.

`linux/stubgen` generates signed stubs and code-free launcher/index/image
splits. Its host harness uses AOSP's `aapt2` to parse generated manifests.
The subsystem device probe is started on a fresh VM with:

```sh
/mnt/data/aosp/out/host/linux-x86/bin/adb -s 127.0.0.1:5556 shell am start \
  -n org.matonos.linux/.stub.ExtraProbeActivity \
  -a org.matonos.linux.DEBUG_EXTRA_PROBE
```

Expected logcat output under `MatonExtraProbe` shows a UID different from the
subsystem app, the regular-file stream contents and `proxy+bwrap exit=0`. The isolated deploy and local
extra-data install tests are documented in the stock-extra final report.
