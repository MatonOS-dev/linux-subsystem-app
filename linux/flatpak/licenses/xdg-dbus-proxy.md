# xdg-dbus-proxy prebuilt provenance

Source: https://github.com/flatpak/xdg-dbus-proxy

Version: 0.1.9; commit `72b40c8f6d9d585cfbdb249690724f740d207087`.
Release archive SHA-256: `5450dda586ec3bb3ca709d311e845487883faa3b09cf562608d7e84f4311dced`.

Built from unmodified upstream source for Android x86_64 / NDK r30 API 35,
against the existing Flatpak NDK GLib/GIO dependency stack. Tests and man
generation disabled. Rebuild with `../build-dbus-proxy.sh`.

Installed as `/system_ext/bin/xdg-dbus-proxy` by `flatpak.mk`'s prebuilt file
list. The Flatpak environment wrapper sets `FLATPAK_DBUSPROXY` to this
runtime path, overriding the old host configure-time probe path.

License: LGPL-2.1-or-later; upstream COPYING is included alongside this file.
