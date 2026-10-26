# Host Flatpak smoke

`run.sh [app-id ...]` installs real Flathub refs in a private user installation,
then uses the production OSTree reader, `ErofsImage`, `StubGenerator` split
writers, `DeploymentRecord`, and `PackageMounts.launchTargets` to prepare and
extract one EROFS image per deployed app/runtime package with `fsck.erofs`.
Extraction preserves symlink metadata on host filesystems where `erofsfuse`
reports symlinks as regular files. The native MatonOS
Flatpak, bwrap, and compat loader execute the real launch under a pre-handler
seccomp filter compiled from `linux/compat-proxy/build/app-policy.h`. Xvfb
serves GUI launches over loopback TCP so bwrap does not bind a host X11 socket
into its synthetic `/tmp`. Launches disable session-bus proxying because the
host has no MatonOS APEX and its host-built `xdg-dbus-proxy` cannot load through
the Flatpak runtime image in the compat loader.

Default refs are Extreme Tux Racer, SuperTuxKart, GNOME Text Editor, Kate,
Obsidian, and Steam. The report and all downloads/build outputs go to
`/mnt/data/aosp/out/pc-logs/host-smoke/`. Override it with
`MATON_HOST_SMOKE_WORK`. Set `MATON_HOST_SMOKE_FLATPAK_USER_DIR` to reuse a
private Flatpak installation outside the work directory. `FSCK_EROFS` may
point to `fsck.erofs`; the default uses the existing erofs-utils 1.9.4 build
under the AOSP output tree.

Run the complete suite with:

```sh
tests/host-smoke/run.sh
```

Run the fast Tux Racer + Text Editor headless subset through `tests/run.sh`:

```sh
MATON_HOST_SMOKE=1 tests/run.sh /mnt/data/aosp/out/pc-logs/host-smoke-tests
```

The Android `PackageInstaller` session and PackageManager signature/split
resolution are not available on a Linux host. The harness writes the same
unsigned base-independent EROFS and index split contents as the production
generators and consumes deployment records and launch targets with the same
production Java code. The host adapter does not invoke Android's
`InstallTransaction`/`StagedPacks` service orchestration: it takes the refs and
deployments from Flatpak's private host installation and supplies them to the
shared OSTree, EROFS, split, record, and launch-target components. It therefore
does not test PackageInstaller/PackageManager signatures or transaction
recovery. The harness extracts generated EROFS images on the host so Linux
preserves symlink metadata; this only replaces device image mounting in the
host adapter. The harness also does not exercise init mount
triggers, Android SELinux, installed seccomp setup, the system bridge, or device
graphics.
