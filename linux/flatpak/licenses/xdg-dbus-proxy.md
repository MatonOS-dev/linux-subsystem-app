# xdg-dbus-proxy prebuilt provenance

Source: https://github.com/flatpak/xdg-dbus-proxy

Version: 0.1.6; commit `1c1989e56f94b9eb3b7567f8a6e8a0aa16cba496`.

Built from unmodified upstream source for Android x86_64 / NDK r30 API 35,
against the existing Flatpak NDK GLib/GIO dependency stack. Tests and man
generation disabled. Rebuild with `../build-dbus-proxy.sh`.

Installed as `/system_ext/bin/xdg-dbus-proxy` by `flatpak.mk`'s prebuilt file
list. The Flatpak environment wrapper sets `FLATPAK_DBUSPROXY` to this
runtime path, overriding the old host configure-time probe path.

License: LGPL-2.1-or-later; upstream COPYING is included alongside this file.
