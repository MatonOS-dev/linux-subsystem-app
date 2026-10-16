# Flatpak binary recipe for the v3.5 subsystem APK

`fetch.sh` verifies `sources.lock`; `build.sh` builds Flatpak/OSTree/bubblewrap
with NDK r30/API 36 and relinks their CLI objects through `make-multicall.py`.
The resulting `matonos-flatpak` is packaged as `libmatonos-flatpak.so`.
Use the repository root `build.sh --build-dependencies` entry point.

Set CACHE_DIR, BUILD_DIR and OUT_DIR outside the source repository. JOBS is
limited to four. The optional START_AT=flatpak reuses prepared dependency
archives and OSTree/bubblewrap objects; the default builds all dependencies.
The final SOURCE contains locked revisions and the binary SHA-256.

Carried from MatonOS_apexs flatpak/; source history for the APK components
comes from the filtered MatonOS device repository. Image-only store applets
and their helper sources are excluded. Per-app launch wrappers are compiled
from ../matonos-app-tool.c by tools/build-flatpak-helpers.sh.
