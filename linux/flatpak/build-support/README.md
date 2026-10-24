# Flatpak binary recipe for the v3.5 subsystem APK

`fetch.sh` verifies `sources.lock` and applies the small tracked
`patches/0001-run-create-private-dev-shm-when-host-lacks-it.patch` to Flatpak's
general device setup. The runner adds a private `--tmpfs /dev/shm` unless the
caller explicitly requests host shared memory or Flatpak's per-app shared
memory feature. The compatibility proxy backs it with session storage under
the app cache. This supplies POSIX shared memory on Android hosts without
`/dev/shm` while retaining Flatpak's explicit sharing choices.

`build-musl.sh` runs the current
`build.sh` recipe inside the offline Alpine 3.24.2 x86_64 root at
`/mnt/data/aosp/out/pc-logs/musl-324`. Flatpak, OSTree, bubblewrap and their
static dependency set are linked with musl as static PIEs, then the Flatpak,
OSTree and bubblewrap CLI objects are relinked through `make-multicall.py`.
The result is packaged as `libmatonos-flatpak.so`; `tools/build-flatpak-helpers.sh`
builds the `libmatonos-app-flatpak.so` launcher with the same static musl

Use the repository root `build.sh --build-dependencies` entry point. The Alpine
root and immutable download cache are shared, while each invocation's build,
plan, helper, and generated output directories live under
`$MATON_SUBSYSTEM_OUT/flatpak/scratch/flatpak-musl` and are bound at
`/scratch/work` inside the sandbox. `MATON_MUSL_SCRATCH` selects the Alpine
3.24.2 root and shared download cache.

The shared `CACHE_DIR` stays under `MUSL_SCRATCH`; `BUILD_DIR` and `OUT_DIR`
stay under the per-invocation work directory. JOBS is limited to four. The
optional START_AT=flatpak reuses prepared dependency
archives and OSTree/bubblewrap objects; the default builds all dependencies.
Builds write generated hash manifests under the output directory and never
rewrite the tracked `prebuilt/static/SOURCE`. After intentionally refreshing
the shipped static prebuilts, run `MATON_SUBSYSTEM_OUT=/path/to/output tools/refresh-flatpak-prebuilt-source.sh`
from the repo root; this explicitly updates the tracked manifest. The
final SOURCE contains locked revisions and SHA-256 hashes for the multicall
and APK helpers. `HELPERS.sha256` records the static musl launcher
recipe and the standalone bwrap artifact hash.

Carried from MatonOS_apexs flatpak/; source history for the APK components
comes from the filtered MatonOS device repository. Image-only store applets
and their helper sources are excluded. Per-app launch wrappers are compiled
from ../matonos-app-tool.c by tools/build-flatpak-helpers.sh. The former
Bionic compatibility header and Android resolver hooks are not part of this
build; installer DNS is handled per process by the compat proxy.
