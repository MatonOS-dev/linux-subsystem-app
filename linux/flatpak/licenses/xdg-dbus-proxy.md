# xdg-dbus-proxy prebuilt provenance

Source: https://github.com/flatpak/xdg-dbus-proxy

Version: 0.1.9; commit `72b40c8f6d9d585cfbdb249690724f740d207087`.
Release archive SHA-256: `5450dda586ec3bb3ca709d311e845487883faa3b09cf562608d7e84f4311dced`.

Built from unmodified upstream source for Android x86_64 / NDK r30 API 35,
against the existing Flatpak NDK GLib/GIO dependency stack. Tests and man
generation disabled. Rebuild with `../build-dbus-proxy.sh`.

Ships inside the `com.matonos.flatpak` APEX as
`/apex/com.matonos.flatpak/bin/xdg-dbus-proxy` (cc_prebuilt_binary in
`linux/flatpak/Android.bp`). The Flatpak environment wrapper sets
`FLATPAK_DBUSPROXY` to this runtime path, and the Flatpak build bakes the same
apex path in via `-Dsystem_dbus_proxy`, overriding the host configure-time
probe path.

License: LGPL-2.1-or-later; upstream COPYING is included alongside this file.
