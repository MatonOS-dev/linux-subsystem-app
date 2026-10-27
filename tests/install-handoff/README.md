# Install hand-off host regression

`tests/run.sh` compiles the real `InstallHandoff`, `DeployManifest`, and
`StubGenerator` sources on the JVM. `InstallTransaction.packageManifest()` now
delegates to `InstallHandoff`, the same seam used by the service path. The test
checks the package document fields consumed by `DeployManifest` and
`StagedPacks`, exercises the row-versus-manifest failure, writes a real binary
APK manifest through `StubGenerator`, and parses it with AOSP `aapt2 dump
badging`. Only Android Keystore signing is replaced with a byte-copy backend;
no test backend is active by default.

It also builds a fake Flatpak `runtime/.../<commit>-en/` deployment with a
deploy record listing `/en`, passes it through the same completed-node row
builder used by `InstallTransaction`, and verifies that the subpath reaches
`InstallHandoff` and resolves to that exact deployment directory. This guards
against dropping locale subsets between pull verification, plan rows, and pack
staging.

For isolated extra-data installs, Flatpak pulls with `--no-deploy`, so scanning
for a deploy record immediately after the stub returns its EROFS image cannot
find the app. The regression starts with that absent record, records a
deployment from the verified pull identity after checkout, and verifies that
`DeployManifest.build` and `verifyPlanned` carry the inspected commit through
the normal staging manifest path.

Package-name validation mirrors
`/mnt/data/aosp/frameworks/base/core/java/android/content/pm/parsing/FrameworkParsingPackageUtils.java`,
`validateName(name, true, true)` (lines 72–103 in this checkout). In
particular each dot starts a segment whose first character must be an ASCII
letter, and the optional filename check limits the name to 255 characters.

The broader `tests/run.sh` also runs the real compatibility-proxy/OSTree
placeholder fixture. The focused hand-off test does not yet invoke Android's
`LinuxAppStoreService.onComplete`, `StagedPacks.build`, or `PackageInstaller`
with an Android `Context`; it verifies their JSON boundary and generated APK
before that platform-only session boundary.
