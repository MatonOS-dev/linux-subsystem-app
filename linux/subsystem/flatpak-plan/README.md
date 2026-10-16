# Flatpak package planning adapter (v3.5)

`inspect.c` is a read-only adapter to the image's pinned Flatpak libraries.
It reads a ref's commit and metadata from one Flatpak remote-state snapshot.
Runtime requirements follow FlatpakTransaction's metadata field rules; related
extensions are selected by Flatpak's own
`flatpak_dir_find_remote_related_for_metadata`, not a second extension resolver.
The metadata mode applies those same rules to another configured remote even
when it does not contain the parent ref. This covers Flatpak's versions,
subdirectories, download-if, no-autodownload and locale selection.

Each invocation sees a private scratch installation with exactly one remote.
The Java planner collects the built-in Flathub origin, signed installed origins
and the requested app origin. Installed refs are authenticated and reused
before provider queries; missing refs use a signed declaring origin when one
exists, otherwise all known remotes are checked and multiple providers require
subsystem-owned confirmation. No payload is pulled until that plan completes.
The pull uses --no-deps and --no-related, verifies the exact planned commit and
metadata, and runs OSTree fsck before packaging. The signed deployment record
stores each dependency's origin and mount for later repair.

`build.py OUTPUT_DIRECTORY` links this small adapter to the already-built,
pinned library dependency set used by MatonOS_apexs (location from paths.md).
It runs no upstream build and changes no upstream sources. Override the built
Flatpak location with MATON_FLATPAK_PLAN_BUILD; MATON_AOSP_ROOT is required.
The Gradle app build invokes it and packages the stripped PIE as
libmatonos-flatpak-plan.so. No output or binary belongs in this directory.
The private Flatpak API is deliberately tied to that pinned build and must be
rechecked when updating Flatpak. The libraries are reused without CLI objects.

Host validation: linux/stubgen/tests/run-tests.sh tests planning, origin-bound
repair, package graphs, mounts, failure handling and signed record round trips.
The native adapter and subsystem APK compile in the normal app build. No VM or
image build is part of this task. Coordinator device checks: reuse an installed
dependency from another origin without any content pull; choose between two
providers; repair a removed dependency from its recorded origin; reject an
untrusted stub occupying the declared package name; launch with nested runtime
extensions. The subsystem's background-confirmation permission and paired
privapp allowlist entry must be included in the image.
