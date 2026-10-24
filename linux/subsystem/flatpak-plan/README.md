# Flatpak package planning adapter (v3.5)

`libmatonos-flatpak-plan.so` is executed by `InstallTransaction` through the
same compat proxy as the Flatpak CLI. Build it as a static musl executable with
`tools/build-flatpak-plan-musl.sh`; that wrapper runs the existing public-API
adapter build against the musl Flatpak tree inside the offline Alpine root.
Android-side code consumes its length-prefixed stdout protocol.

`inspect.c` uses only Flatpak's public `<flatpak.h>` API. It fetches remote
metadata and refs through `FlatpakInstallation`, and asks
`flatpak_installation_list_remote_related_refs_sync` for extensions. The
planner receives the same length-prefixed binary response as before. Runtime
requirements are read from the fetched (or supplied local) `GKeyFile`;
extension download decisions and subpaths come from public
`FlatpakRelatedRef` getters.

Each invocation sees a private scratch installation with exactly one remote.
The Java planner collects the built-in Flathub origin, signed installed origins
and the requested app origin. Installed refs are authenticated and reused
before provider queries; missing refs use a signed declaring origin when one
exists, otherwise all known remotes are checked and multiple providers require
subsystem-owned confirmation. No payload is pulled until that plan completes.
The pull uses `--no-deps` and `--no-related`, verifies the exact planned commit
and metadata, and runs OSTree fsck before packaging.

`build.py OUTPUT_DIRECTORY` compiles this adapter against the pinned Flatpak
build and its dependency link set. It includes only `<flatpak.h>` from Flatpak;
no private headers or private API symbols are used. Override the build location
with `MATON_FLATPAK_PLAN_BUILD`; `MATON_AOSP_ROOT` is required. The Gradle app
build packages the stripped PIE as `libmatonos-flatpak-plan.so`.

Host validation: `linux/stubgen/tests/run-tests.sh` covers planning,
origin-bound repair, package graphs, mounts, failure handling and signed record
round trips. The subsystem app build compiles the native adapter.

For the Steam multiarch regression, run
`python3 linux/subsystem/flatpak-plan/tests/compat_i386.py <plan-output>/libmatonos-flatpak-plan.so`.
Steam's signed app metadata declares Compat.i386 and GL32, while those
x86_64 extension refs' metadata names an unavailable i386 parent runtime.
Planning keeps the app's declared x86_64 runtime and leaves the extensions to
Flatpak's related-ref resolver. Mount construction resolves their directory
from the app's authenticated extension declaration and requires its declared
x86_64 runtime to already be staged.
