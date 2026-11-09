# D-Bus tray provider

`org.matonos.tray` provides `org.kde.StatusNotifierWatcher`. Each
`IDBusService.attach()` owns a separate seqpacket connection and item map, so
registrations from one stub bus are not visible to another. The provider handles
item and host registration, watcher properties, owner loss, item property
refresh, pixmap decoding, item action forwarding, and DBusMenu layout and event
calls.

The home-screen AppWidget renders non-Passive items from every attached bus in a
horizontal strip. It prefers `IconPixmap`; until icon-theme lookup exists, it
shows `IconName` as text. NeedsAttention items get a badge. Tapping an item calls
`Activate(0,0)`; ItemIsMenu items and items returning UnknownMethod open the
menu popup.

The dialog-themed menu Activity keeps DBusMenu submenus hierarchical. It
renders enabled/visible state, separators, check and radio toggles, and
`icon-data`. It calls AboutToShow before opening the root and each submenu,
sends Event `clicked` for selected leaves, and refreshes the currently open menu
when LayoutUpdated or ItemsPropertiesUpdated arrives. Outside taps dismiss the
popup. The older D-Bus value reader, writer, signature parser, and message codec
are shared under `host/shared/src/main/java/org/matonos/dbus`.

## Build and tests

From the repository root, use the regular `./build.sh` environment from the
bridge task brief. It assembles `:dbus-tray` and runs
`:dbus-tray:testDebugUnitTest`. JVM tests cover pixmap/menu wire decoding and
the menu-tree view model, including nested submenus, toggle states, and hidden
items.

No device or VM install was done for this task. On a device with the bridge
consumer enabled, add the D-Bus Tray widget, start Linux apps that register SNI
items on separate stub buses, and verify both appear. Check icons and the
NeedsAttention badge; tap a normal item and confirm Activate, then tap a menu
item and confirm hierarchical submenus, toggles, separators, disabled/hidden
items, and leaf Event delivery. Update menu properties while open and confirm
the current submenu refreshes. Stop each Linux app and confirm only its items
disappear after its bus name is released.

## Open issues

- Icon theme name resolution is intentionally absent; only `IconPixmap` is
  rendered as an image.
- The popup is positioned at the bottom center because Android does not provide
  the widget's screen bounds in a normal widget click PendingIntent.
- The widget and popup have not been exercised against a live Linux tray
  implementation or the Android binder bridge.
