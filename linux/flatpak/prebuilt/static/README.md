# Bionic Flatpak tool inputs

The directory name is retained for build-input stability. The payload is Bionic.
`tools/build-flatpak-helpers.sh` builds the Bionic `matonos-app-flatpak`
entry point. `HELPERS.sha256` fingerprints its compiler, flags and included
sources, and records the standalone bwrap artifact hash. `matonos-app-bwrap`
is bubblewrap 0.13.0 built as a static PIE with the pinned musl toolchain, so
its syscalls never pass through Android libc wrappers. `SOURCE` records the
pinned multicall sources, the `matonos-flatpak` hash, and bwrap's recipe and
artifact hashes.

Build with `linux/flatpak/build-support/fetch.sh` and `build.sh`, keeping cache,
build and output directories outside this repository. Copy the verified
`OUT_DIR/matonos-flatpak` to `x86_64/` and `OUT_DIR/SOURCE` here, or use the root
`./build.sh --build-dependencies` to do that as part of a signed APK build.
The fixed runtime prefix, runtime path audit and two-directory comparison are
in [REPRODUCIBILITY.md](../../build-support/REPRODUCIBILITY.md). Rebuild bwrap
with `linux/flatpak/build-support/build-bwrap-musl.sh` and the pinned bubblewrap
source tree and musl build root.

No legacy store, portal or fallback launch artifacts are shipped.
