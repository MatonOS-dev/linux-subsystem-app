# Bionic APEX payload staging

The directory name is retained for build-input stability. The payload is bionic.
`tools/build-flatpak-helpers.sh` builds flatpak-env-wrapper (management only),
matonos-flatpak-store (signed-ref staging) and matonos-app-tool (installed as
matonos-app-flatpak and matonos-app-bwrap). HELPERS.sha256 fingerprints their
compiler, flags and included sources. SOURCE records upstream multicall/portal
provenance; later in-tree artifact hashes supersede the original helper hashes.

MatonOS_apexs produces matonos-flatpak and matonos-flatpak-portal.
The old bwrap/app-exec binaries and SysV preload libraries are deleted.
No fallback launch artifacts are shipped.
