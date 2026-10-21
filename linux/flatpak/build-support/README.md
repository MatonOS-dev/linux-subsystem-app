# Flatpak binary recipe for the v3.5 subsystem APK

`fetch.sh` verifies `sources.lock`; `build-musl.sh` runs the current
`build.sh` recipe inside the offline Alpine 3.24.2 x86_64 root at
`/mnt/data/aosp/out/pc-logs/musl-324`. Flatpak, OSTree, bubblewrap and their
static dependency set are linked with musl as static PIEs, then the Flatpak,
OSTree and bubblewrap CLI objects are relinked through `make-multicall.py`.
The result is packaged as `libmatonos-flatpak.so`; `tools/build-flatpak-helpers.sh`
builds the `libmatonos-app-flatpak.so` launcher with the same static musl
toolchain. Both keep their installed names for APK compatibility.

Use the repository root `build.sh --build-dependencies` entry point. Source,
build and output directories stay under the Alpine scratch tree; set
`MATON_MUSL_SCRATCH` to use another Alpine 3.24.2 root/scratch pair.

Set CACHE_DIR, BUILD_DIR and OUT_DIR under `MUSL_SCRATCH`. JOBS is limited to
four. The optional START_AT=flatpak reuses prepared dependency
archives and OSTree/bubblewrap objects; the default builds all dependencies.
The final SOURCE contains locked revisions and SHA-256 hashes for the
multicall and APK helpers. `HELPERS.sha256` records the static musl launcher
recipe and the standalone bwrap artifact hash.

Carried from MatonOS_apexs flatpak/; source history for the APK components
comes from the filtered MatonOS device repository. Image-only store applets
and their helper sources are excluded. Per-app launch wrappers are compiled
from ../matonos-app-tool.c by tools/build-flatpak-helpers.sh. The former
Bionic compatibility header and Android resolver hooks are not part of this
build; installer DNS comes from the existing per-app musl DNS forwarder.
