# Bionic Flatpak tool inputs

The directory name is retained for build-input stability. The payload is Bionic.
`tools/build-flatpak-helpers.sh` builds the APK helper entry points, installed as
matonos-app-flatpak and matonos-app-bwrap. `HELPERS.sha256` fingerprints their
compiler, flags and included sources. `SOURCE` records the pinned multicall
sources and the SHA-256 of `x86_64/matonos-flatpak`.

Build with `linux/flatpak/build-support/fetch.sh` and `build.sh`, keeping cache,
build and output directories outside this repository. Copy the verified
`OUT_DIR/matonos-flatpak` to `x86_64/` and `OUT_DIR/SOURCE` here, or use the root
`./build.sh --build-dependencies` to do that as part of a signed APK build.
The fixed runtime prefix, runtime path audit and two-directory comparison are
in [REPRODUCIBILITY.md](../../build-support/REPRODUCIBILITY.md).

No legacy store, portal or fallback launch artifacts are shipped.
