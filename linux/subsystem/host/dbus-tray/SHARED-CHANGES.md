# Shared change needed for tray-fix-3

The tray cannot implement `IconThemePath` resolution safely with the current
subsystem APIs. The existing Linux-to-host path translation is performed by
the launcher's bwrap mount arguments (`/app` and `/usr` map to mounted EROFS
trees); `HOME` maps to `<stub ApplicationInfo.dataDir>/home` in
`V3Launcher`. There is no reusable host-side resolver or open-file API.

The tray runs as `org.matonos.tray`, while each Flatpak stub has its own
Android UID. In particular, a tray-side `File` mapping of `/home/...` to the
stub's `dataDir/home` is not readable across that UID boundary. Directly
mapping paths in the tray would therefore fail for private app data and would
duplicate the launcher's EROFS mapping logic.

Needed shared API: a narrow, read-only per-stub file resolver that accepts a
Linux-visible absolute path and returns an opened file descriptor (or a clear
not-found/unreadable result). It must run in the owning stub UID, reuse the
launcher's established `/app`, `/usr`, and `HOME` mappings, reject paths that
escape those roots, and be callable by the signed tray app. Then dbus-tray can
read `IconThemePath` and resolve icons without changing `broker.c`.

No Steam D-Bus capture or source showing the concrete `IconThemePath` value is
present in this checkout or the referenced task logs, so the actual path Steam
sends has not been verified here.
