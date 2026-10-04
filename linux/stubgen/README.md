# Image-carrying launcher APKs

MatonOS is based on AOSP. StubGenerator remains Java and retains its existing
no-image generate signature. Its new overload accepts a file containing the
code EROFS image and an optional RuntimeDependency. generateRuntime creates a
code-free static-library APK; generateExtra creates the code-free `extra` split.
No AOSP changes, native components, kernel changes or SELinux changes are used.

Images are streamed twice (CRC, then ZIP output), stored as the first ZIP entry,
and padded through a local extra field to a 4096-byte data offset. Signing uses
alignment preservation. Every image generation checks the signed output with
checkImageEntry, which checks local and central records, matching sizes/names,
compression/descriptor/encryption flags, bounds, single-image identity, alignment,
and ZIP64 sizes, offsets and end records without loading the image into memory.
Image content must already be EROFS; this component does not construct or mount it.

Runtime package names follow the contract's sanitization exactly. The generator
accepts positive int versions (the manifest's framework version attribute is an
int). The bridge persists a per-ref monotonically increasing reservation counter
through reserveFlatpakRuntimeVersion; reserve before generating a new runtime,
and never reuse a reservation for new content. runtimeDependency obtains the
SHA-256 digest from the same signing key. APK generation must run in the existing
bridge key's UID: AndroidKeyStore aliases are scoped by UID, so another app using
this library cannot impersonate the bridge's per-device stub key.

The appended AIDL methods are scoped to the existing `flatpak` installer grant.
installFlatpakImagePackages copies supplied APK FDs into private scratch, verifies
the signer, stub ref/package, runtime declaration and dependency/version/digest,
and commits runtime first. The app is committed only on runtime success.
installFlatpakExtra verifies the existing stub and extra manifest, then commits
MODE_INHERIT_EXISTING. PackageInstaller verifies its signer against the base.
Methods return session IDs, not install success; failures are logged. Scratch is
removed after completion. The original reconciliation and no-image path remain.

getFlatpakImagePackages accepts linuxd's system UID or a caller with the existing
flatpak_launch grant, verifies the specified stub UID/ref/signer, and returns
code/optional-extra/runtime lines from PackageManager. Runtime lookup requests
MATCH_STATIC_SHARED_AND_SDK_LIBRARIES and verifies its signer and public declaring
package. Paths must be under /data/app/ and exclude traversal, repeated slashes
and controls. Every referenced APK also passes the image checker. linuxd remains
responsible for writing the root-owned launch record; it was not modified here.

## Tests

Run from this device-tree worktree:

```
bash linux/stubgen/tests/run-tests.sh "$AOSP_ROOT" "$AOSP_ROOT/out/pc-logs/apk-images"
```

The tests compile against the existing SDK/apksig, sign with a disposable host
certificate, verify signatures and signed alignment, parse all three manifests
with the checkout's aapt2, reject malformed headers/uncompressed-but-unaligned,
descriptor/compression/size/offset errors and multiple images, exercise ZIP64
entry extras plus ZIP64 end/locator records, and retain the no-image resource/DEX
behavior. Existing permission metadata tests also pass. Disposable private keys
are deleted. Outputs stay outside source projects. The tests use small arbitrary
payloads to check ZIP layout; they do not claim to validate EROFS content.

Host javac also compiled FlatpakStubManager and SystemBridgeService against the
existing platform framework/bridge jars and newly generated AIDL. This is not a
Soong build or device-runtime test. Shared checkout preflight passes; worktree
preflight fails because git-ignored app/native prebuilts are absent (and its older
checker flags a module-reference source). No new build module names were added.

## Required image and hardware verification

The coordinator must integrate/review this worktree, run preflight and build
MatonLinuxStubGenerator and MatonSystemBridge, then package a fresh image. Do not
start an AOSP build from this worktree or test by replacing installed system files.

On a fresh image in one headless QEMU VM (port 5556+, <=4096 MiB), disable sleep
idle and use a trusted installer caller to reserve a runtime version, generate
runtime/app APKs with the bridge's key and real EROFS files, and install them.
Confirm runtime success precedes the app session; inspect PM's static-library
link and the resolved launch record. Install and replace extra; verify the base
and its grants survive. Reject a wrong signer/ref/runtime digest/split/version.
Verify absent extra produces exactly two record lines. Test an old no-image stub
still launches. Run tools/check-selinux-labels.sh on the built image. Stop the VM.
Repeat those package/launch checks on Ryzen 5800X/RX6600, Surface Pro 3 and HP
ProDesk 600 G1: there is no hardware-specific behavior in this component.

Open issues: no fresh image was built or booted for this worktree yet; real
PackageInstaller/static-library resolution remains unverified. Images >4 GiB and
actual EROFS mounts are not tested. Existing linuxd/store integration must adopt
the new APIs (outside this task); their launch-record writing is unchanged. The
contract references a "Planned: images carried in APKs" section missing from
this worktree's CODE-STORAGE-r24.md; the explicit apk-images-contract.md was used
unchanged. No feature was dropped for requiring AOSP patches.
