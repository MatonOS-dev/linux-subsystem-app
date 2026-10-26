# Host Flatpak smoke

`run.sh [app-id ...]` installs real Flathub refs in a private user installation,
then uses the production OSTree reader, `ErofsImage`, `StubGenerator` split
writers, `DeploymentRecord`, and `PackageMounts.launchTargets` to prepare and
mount one EROFS image per deployed app/runtime package. The native MatonOS
Flatpak, bwrap, and compat loader execute the real launch under a pre-handler
seccomp filter compiled from `linux/compat-proxy/build/app-policy.h`. Xvfb
serves GUI launches over loopback TCP so bwrap does not bind a host X11 socket
into its synthetic `/tmp`. Launches disable session-bus proxying because the
host has no MatonOS APEX and its host-built `xdg-dbus-proxy` cannot load through
the Flatpak runtime image in the compat loader.

Default refs are Extreme Tux Racer, SuperTuxKart, GNOME Text Editor, Kate,
Obsidian, and Steam. The report and all downloads/build outputs go to
`/mnt/data/aosp/out/pc-logs/host-smoke/`. Override it with
`MATON_HOST_SMOKE_WORK`. An optional `EROFS_FUSE` points to `erofsfuse`; the
default uses the existing erofs-utils 1.9.4 build under the AOSP output tree.

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
production Java code. `erofsfuse` mounts generated images read-only, but its
1.9.4 FUSE path can expose EROFS symlinks as regular files containing their
target text; failures at those paths are host filesystem gaps, not evidence of
a device launch failure. The harness also does not exercise init mount
triggers, Android SELinux, installed seccomp setup, the system bridge, or device
graphics.
