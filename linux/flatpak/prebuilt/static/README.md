# Static musl Flatpak tool inputs

The directory name is retained for APK path stability. All three shipped
executables are static musl binaries: the `matonos-flatpak` Flatpak/OSTree/bwrap/portal multicall,
the `matonos-app-flatpak` per-app launcher, and `matonos-app-bwrap` bubblewrap
0.13.0. Their Linux syscalls therefore do not enter Android's Bionic wrappers.
`HELPERS.sha256` fingerprints the launcher compiler and source and records the
bwrap recipe/artifact hash. `SOURCE` records the locked multicall sources and
the hashes of all three installed executables. The generated
`libmatonos-flatpak-plan.so` helper is also a static musl executable; Gradle
builds it from the same static Flatpak tree through
`tools/build-flatpak-plan-musl.sh`.

Build with `linux/flatpak/build-support/fetch.sh` and
`linux/flatpak/build-support/build-musl.sh`, keeping cache, build and output
directories under the external Alpine scratch tree. The root
`./build.sh --build-dependencies` runs the same steps before a signed APK build.
The fixed runtime prefix, runtime path audit and two-directory comparison are
in [REPRODUCIBILITY.md](../../build-support/REPRODUCIBILITY.md). Rebuild bwrap
with `linux/flatpak/build-support/build-bwrap-musl.sh` and the pinned bubblewrap
source tree and musl build root.

No legacy store, portal or fallback launch artifacts are shipped.
