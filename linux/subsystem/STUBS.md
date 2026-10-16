# MatonOS Flatpak stub APK format v1

A stub APK is a small, signed package record for one installed Flatpak app. It
contains no application implementation. The shared code is supplied by the
updatable `org.matonos.linux` host through Android's dynamic shared
library mechanism. The stub APK is signed on-device with the per-device key
held by Android Keystore; it is not portable to another device.

## Contents

The v1 package contains:

- A binary `AndroidManifest.xml` with the appstream display label, the required
  `<uses-library android:name="org.matonos.linuxhost"/>`, approved Android
  permission declarations, a launcher activity whose class is
  `org.matonos.linux.stub.StubActivity` (shipped `android:enabled="false"`;
  the bridge enables it once the Flatpak install completes, so the app stays
  hidden until then), an always-enabled, unprivileged
  `org.matonos.linux.stub.InstallActivity` ("install me": the store uses
  it to resume an interrupted install; no-op when installed), and a placeholder service class
  `org.matonos.linux.stub.StubService`.
- An `ACTION_MAIN` / `CATEGORY_LAUNCHER` filter. Stubs declare no
  `ACTION_VIEW` MIME filters: the launcher label and icon come from the
  remote's appstream data, not the deployment's exported `.desktop` file, so
  its `MimeType=` entries are never read.
- Activity metadata `org.matonos.linuxhost.FLATPAK_REF` containing the exact
  installed ref, `org.matonos.linuxhost.FLATPAK_REMOTE` containing the
  configured install remote, and `org.matonos.linuxhost.MIN_INTERFACE_VERSION`
  containing the minimum host interface version (currently `1`). The ref
  supplies app ID, architecture, and branch; Flatpak resolves the runtime and
  dependencies from app metadata.
- A minimal empty `resources.arsc` table required by Android PackageManager
  even though the stub declares no compiled resources.
- The caller-provided PNG at `res/drawable/foreground.png` — the launcher icon
  from the remote's appstream data — an adaptive icon XML, and an empty, valid
  `classes.dex`.

Package names are selected by the caller and must be unique Android package
names. Stub format version is `1`; it is separate from the host interface
version. The Flatpak ref is checked against the standard
`app|runtime/name/arch/branch` form. The launcher label and icon are supplied
by the store from the remote's appstream data; the generator never reads the
deployment's `.desktop` file.

## Host interface

`HostContract.INTERFACE_VERSION` is the host ABI version. The host launcher
reads the ref and minimum version from the stub activity metadata. It routes
to the compositor host Activity and presents an explicit not-wired message
until the Flatpak runtime launch hook is connected. `StubService` is reserved
for future per-app D-Bus and portal integration; it does not implement those
features in v1.

The stub generator is `MatonLinuxStubGenerator` in `linux/stubgen`. Its Java
entry point accepts an app-private work directory, Flatpak ref, launcher label, PNG bytes,
permission list, package name, and output APK file. It encodes the small
binary XML vocabulary directly, so it does not require AAPT2 on-device, then
signs with `apksig`. The Android Keystore alias
`matonos_flatpak_stub_v1` is created on first use as a non-exportable EC
P-256 signing key.

## Stock PackageManager multiarch handling

The first on-device attempt used a host APK without `android:multiArch` and
PackageManager rejected that library provider because it contained JNI
libraries. AOSP's `PackageAbiHelperImpl` handles `pkg.isMultiArch()` in a
separate ABI path before the single-ABI branch that throws
`Shared library with native libs must be multiarch`. The host manifest now
sets `<application android:multiArch="true"/>`; AAPT2 confirms the flag is
present. A fresh-image build and stub-install retest are pending. If the
multiarch APK still cannot register as a shared-library provider, use the
Java-only provider/native-engine split documented as a contingency in
[SHARED-CHANGES.md](SHARED-CHANGES.md), without patching AOSP.

## Verification status

On a fresh QEMU boot, the generator produced a signed APK using Android
Keystore. The fixed manifest parsed through reconciliation. That image's host
APK did not yet declare `android:multiArch="true"`, so PackageManager did not
register the dynamic library and the stub failed with
`INSTALL_FAILED_MISSING_SHARED_LIBRARY`. The rebuilt host APK now sets the
flag; install and launcher verification on an image containing it are pending.

After the provider/engine split, repeat on a freshly built and booted image:

1. Expose `MatonLinuxStubGenerator` to a small trusted test caller (the store
   app or bridge; integration is out of scope here).
2. Generate a stub for `app/org.example.Test/x86_64/stable` with an appstream
   label, PNG, and no permissions.
3. Install it with `pm install -r /data/local/tmp/test.apk`, verify `pm path`
   and dynamic-library resolution, enable it with `pm enable
   <package>/org.matonos.linux.stub.StubActivity`, then launch its
   launcher activity.
4. Confirm the host shows the explicit “Flatpak runtime launcher is not
   wired” message until the runtime hook is connected.

## Follow-up contract for the bridge and store

The store must choose the package name, map requested Flatpak permissions to
the approved Android permission list, provide the appstream launcher label and
icon bytes, and invoke the generator when installing or updating a ref. The bridge/store integration also needs a narrow caller
surface for this library and policy for which signed callers may mint stubs.
The installed per-device key must not be exported or copied; after a reset,
stubs are re-minted from installed refs and store metadata.
