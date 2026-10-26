# Host Flatpak smoke

`run.sh [app-id ...]` installs real Flathub refs in a private user installation,
then uses the production OSTree reader, `ErofsImage`, `StubGenerator` split
writers, `DeploymentRecord`, and `PackageMounts.launchTargets` to prepare and
extract one EROFS image per deployed app/runtime package with `fsck.erofs`.
The host adapter restores library, executable, and app link-farm symlinks from
each Flatpak deployment after extraction, because the host OSTree conversion
can flatten them into regular files containing their target text. The native MatonOS
Flatpak, bwrap, and compat loader execute the real launch under a pre-handler
seccomp filter compiled from `linux/compat-proxy/build/app-policy.h`. Xvfb
serves GUI launches over loopback TCP so bwrap does not bind a host X11 socket
into its synthetic `/tmp`. The host-built `xdg-dbus-proxy` and its host GLib
dependencies run with the host loader from a private APEX path, leaving the
Flatpak runtime's loader and libraries intact. Flatpak uses the current host
user bus when available; otherwise the harness creates one with
`dbus-run-session`.

For FreeDesktop runtimes, the harness also materializes the subsystem APK's
`GL.maton` extension (and `GL32.maton` for apps with the i386 compatibility
runtime) into a temporary Flatpak system installation, then maps those files
to the same extension paths used by the device launcher. It sets
`FLATPAK_GL_DRIVERS=maton` so Flatpak selects the forwarder ICD and uses
`--nodevice=all` to keep host devices, including `/dev/dri`, out of the app.
`MATON_HOST_SUBSYSTEM_APK` can override the default subsystem APK path.

Default refs are Extreme Tux Racer, SuperTuxKart, GNOME Text Editor, Kate,
Obsidian, and Steam. The report and all downloads/build outputs go to
`/mnt/data2/matonos-host-smoke/`. Override it with
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
MATON_HOST_SMOKE=1 tests/run.sh /mnt/data2/matonos-host-smoke/host-smoke-tests
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
