# D-Bus tray provider

`org.matonos.tray` provides `org.kde.StatusNotifierWatcher`. Each
`IDBusService.attach()` owns a separate seqpacket connection and item map, so
registrations from one stub bus are not visible to another. The provider handles
item and host registration, watcher properties, owner loss, item property
refresh, pixmap decoding, item action forwarding, and DBusMenu layout and event
calls.

The home-screen AppWidget renders non-Passive items from every attached bus in a
horizontal strip. Icon selection is `IconName` from the active theme, then
`IconPixmap`, then the attached Android package's launcher icon, then text. The
tray records the attach UID and resolves its package with
`PackageManager.getPackagesForUid()`. Flatpak packages use the subsystem's
`flatpak.<kind>.<id>.<arch>.<branch>` package naming; the app ID segment is
decoded by converting encoded underscores to dots for the exact
`<app-id>-symbolic` theme lookup. `DeployManifest` encodes both dots and
literal underscores as underscores, so this package-derived ID can be
ambiguous for IDs containing literal underscores; the launcher icon fallback
still works in that case. If the theme icon is absent, an adaptive icon's
monochrome drawable is used, or its launcher drawable is reduced to an alpha
silhouette. These symbolic fallbacks use the regular symbolic tint treatment.
Launcher icon results are cached per UID and cleared on package add, remove, or
replacement. PackageManager returns the known owner packages by UID, so no
package visibility query is needed.

Menu popups use a dedicated task affinity and a singleTask activity, so they
open outside the debug-list task and a subsequent item tap reuses the popup
activity with the new item and anchor bounds.
NeedsAttention items get a badge. Tapping an item calls
`Activate(0,0)`; ItemIsMenu items and items returning UnknownMethod open the
menu popup.

The menu Activity is a compact dark popup positioned above the clicked item,
with its left edge aligned to that item's screen bounds and clamped to the
display. It renders a muted app-title header, hover highlighting, enabled
state, separators, check switches, radio indicators, themed or supplied row
icons, and right-aligned shortcuts. DBusMenu submenus remain hierarchical and
open as nested popup sections with a Back row. It calls AboutToShow before
opening the root and each submenu, sends Event `clicked` for selected leaves,
and refreshes the currently open menu when LayoutUpdated or
ItemsPropertiesUpdated arrives. Outside taps, Escape, and focus loss dismiss
the popup. The older D-Bus value reader, writer, signature parser, and message
codec are shared under `host/shared/src/main/java/org/matonos/dbus`.

Desktop launchers can host a live tray pill through `ITrayHost.attachPill()`.
It returns a `SurfaceControlViewHost.SurfacePackage` for the supplied display
and host token. The hosted view occupies the full supplied slot height while
the capsule and icon slots are vertically centered inside it; the caller installs it with
`SurfaceView.setChildSurfacePackage()`. The pill draws registered non-Passive
items in registration order, tints icons white, marks NeedsAttention, and
reports content width changes through `IPillCallback.onWidthChanged()`. Call
`setPillOrigin(screenX, screenY)` after layout so item menu bounds use screen
coordinates, then call `detachPill()` when the host surface goes away. Primary
click opens the existing Activate flow, secondary click or long press opens
the item's menu, and a swipe over an item sends Scroll. The older Bundle
listener and activate/openMenu calls remain available for Launcher3.

Launcher splits include an adaptive icon monochrome layer when the exported
icon tree provides the exact `<app-id>-symbolic` asset. PNG and SVG inputs use
the same rasterization path as the regular launcher icon. Without that asset,
the generated adaptive icon remains foreground and background only.

Each registered StatusNotifierItem also has an ongoing notification in the
`tray_items` low-importance channel. Notification icons use the active theme,
the item's pixmap, then the attached package's symbolic/monochrome launcher
icon or alpha silhouette, rendered white on transparency. The pill uses the
same cached resolver, so launcher fallbacks also appear there. Icon changes,
theme changes, and package changes invalidate per-item icon caches. The
notification title uses the SNI title or app label, its text uses the tooltip,
and NeedsAttention adds a visible subtext while remaining silent. Tapping
activates the item; items with a menu also have a Menu action that opens the
existing popup near the top of the screen when no anchor is available. The app
declares `POST_NOTIFICATIONS`; when permission is missing, it logs the reason
once and continues serving the pill.

The subsystem app exposes installed icon-theme files through the read-only
`org.matonos.linux.icons` ContentProvider. It finds runtime icon-theme extension
packages from their installed Flatpak ref metadata, mounts their EROFS image
through `matonos-erofsd`, and opens the selected file descriptor. The exported
provider requires `org.matonos.permission.READ_ICON_THEMES` (signature). The
tray app and subsystem currently share a signing key. The tray bundles
AndroidSVG 1.4 (Apache-2.0) to render installed SVG files; see
[third party notices](THIRD_PARTY_LICENSES.md). No Papirus files are bundled.

The lookup follows `index.theme` directory sizes, scale, thresholds, scalable
ranges, inheritance, and `hicolor`; it tries `<name>-symbolic` first. Provider
queries also list installed themes. Unknown icons return no file. Parse and I/O
failures are logged with theme and path. The tray caches by theme, name, and
pixel size, and clears that cache on installed-package changes.

In the v3 layout, each Flatpak ref is installed as an Android package with one
EROFS image split. `matonos-erofsd` mounts that image at
`/mnt/matonos/<owner uid>/<n>` while a Binder token is held. Runtime extension
refs can be installed as standalone non-launchable packages through the normal
store install flow; the store generates an APK carrying the ref metadata and
the image split. This checkout no longer has the retired shared
`/data/linux-app` deployment store; PackageManager owns each installed ref
package, while active mounts live under `/mnt/matonos/<owner uid>/<n>`. The
provider also reads the matching Platform runtime image for inherited
`hicolor` icons. The subsystem provider is the only component here that reads
the mounted images. Tray never opens those paths.

## Build and tests

From the repository root, use the regular `./build.sh` environment from the
bridge task brief. It assembles `:dbus-tray` and runs
`:dbus-tray:testDebugUnitTest`. JVM tests cover pixmap/menu wire decoding,
popup placement clamping, the menu-tree view model, and notification text
mapping. `AppIconResolverTest` covers the theme → item pixmap → launcher
fallback order.
`IconThemeLookupTest` builds a small fake theme tree and covers
fixed/scalable sizes, scale, threshold, inheritance, hicolor fallback, and
symbolic preference.

No device or VM install was done for this task. On a device with the bridge
consumer enabled, add the D-Bus Tray widget, start Linux apps that register SNI
items on separate stub buses, and verify both appear. Check icons and the
NeedsAttention badge; tap a normal item and confirm Activate, then tap a menu
item and confirm hierarchical submenus, toggles, separators, disabled/hidden
items, and leaf Event delivery. Update menu properties while open and confirm
the current submenu refreshes. Stop each Linux app and confirm only its items
disappear after its bus name is released.

For pill verification, bind to `org.matonos.tray.HOST`, attach a `SurfaceView`
using its display ID, host token, and available height, then set the screen
origin after layout. Add and remove tray items while attached and verify the
reported width and drawn item order update. Check primary activation,
secondary click, long press, scroll, and anchored menu bounds. Detach and
reattach the host surface and confirm updates continue.

For icon-theme verification, install the Papirus extension as a standalone
Flatpak ref for the installed Platform branch through the normal store install
path, then start a tray item that sends an `IconName` available in Papirus.
Confirm the provider lists Papirus and the widget shows that icon; include a
symbolic icon to verify its white tint. Remove the extension and confirm a
subsequent package change clears the tray cache and falls back to the item's
pixmap or text. No extension is installed automatically.

## Open issues

- The popup uses source bounds supplied by the launcher or tray host; callers
  without screen bounds use the bottom-center fallback.
- The provider and tray have not been exercised with a Papirus extension
  installed on a device or against a live Linux tray implementation.
