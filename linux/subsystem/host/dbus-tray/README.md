# D-Bus tray provider

`org.matonos.tray` is a debug provider for `org.kde.StatusNotifierWatcher`.
Each `IDBusService.attach()` owns a separate seqpacket connection and item map,
so registrations from one stub bus are not visible to another.

The service handles item and host registration, watcher properties, item
owner loss, StatusNotifierItem property refresh, pixmap decoding, item action
forwarding, and the DBusMenu `GetLayout`, `AboutToShow`, and `Event` calls.
Icon names remain as model strings; theme lookup is not implemented. The
Activity is a plain diagnostic list with a menu dialog and is not a launcher
widget.

The D-Bus value reader, writer, signature parser, and message codec are shared
under `host/shared/src/main/java/org/matonos/dbus`; `dbus-echo-test` now uses
the same codec.

## Build and tests

From the repository root, use the regular `./build.sh` environment from the
bridge task brief. It builds `:dbus-tray:assembleDebug` and runs
`:dbus-tray:testDebugUnitTest`. The JVM tests round-trip `a(iiay)` icon pixmaps
and nested `ia{sv}av` DBusMenu layouts.

No device or VM install was done for this task. On a device with the bridge
consumer enabled, start a Linux app that registers an SNI item, open D-Bus
Tray Debug, check title/icon/status, open its menu, and tap an enabled leaf.
Then stop the Linux app and confirm the item disappears after its bus name is
released. Repeat from two stubs to check that their item lists remain separate.

## Open issues

- Icon theme name resolution is intentionally absent; only `IconPixmap` is
  rendered.
- The debug Activity is not a home-screen widget and has not been exercised
  against a live Linux tray implementation or the Android binder bridge.
- Menu submenus are flattened into one diagnostic list; no launcher UI is
  included.
