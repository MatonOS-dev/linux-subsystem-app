# Per-stub D-Bus broker core

`matonos-dbus-broker` is a small per-stub session-bus broker. It uses GLib's
GDBus server transport and manually supplies the bus name registry, policy,
message routing, and signal match routing. Each app's processes connect to
the socket owned by that stub; the server requires same-user EXTERNAL
authentication and checks peer credentials against the UID that launched
the broker.

## Build and run on a host

Requires a C compiler, `pkg-config`, and GLib/GIO development files:

```sh
make
make test
./matonos-dbus-broker /run/user/10000/matonos-bus.sock policy.conf
```

The default Makefile output is under the AOSP `out/pc-logs/dbus-broker/build`
directory. For another checkout, pass `OUT=/absolute/path`. A policy file is
plain text, with one `own NAME` or `talk NAME` rule per line; comments start
with `#`. Names not listed are denied. `org.freedesktop.Flatpak`,
`org.freedesktop.systemd1`, and PackageKit names are reserved and rejected.

The broker also claims the internal service names registered through
`broker_add_service()`. Such hooks are exported on each connected peer, so
the later portal implementation can be broker-owned code without a separate
daemon. The included `org.matonos.Test.Echo` hook is only a smoke-test service.

## Current protocol coverage

- EXTERNAL auth on a Unix socket, same-UID peer enforcement, unique names,
  `Hello`, `RequestName`, `ReleaseName`, `GetNameOwner`, `NameHasOwner`,
  `ListNames`, `ListActivatableNames`, `AddMatch`, `RemoveMatch`,
  `GetConnectionUnixUser`, `GetConnectionUnixProcessID`, and `GetId`.
- `StartServiceByName` is implemented but never activates an unknown service:
  it is policy checked and returns `ServiceUnknown` for configured names that
  have no owner.
- `NameOwnerChanged`, `NameAcquired`, and `NameLost`; Peer `Ping` and
  `GetMachineId`; Introspectable `Introspect`.
- Default-deny `own`/`talk` checks; unicast request/reply/error routing and
  broadcast signal delivery for supported match fields (`type`, `sender`,
  `interface`, `member`, `path`, `destination`, and string `argN` /
  `argNnamespace`).
- All Flatpak and systemd bus names and PackageKit bus names are denied.
- Set `MATONOS_DBUS_TRACE=1` on the broker to log every inbound method call
  (sender, destination, path, interface, member, and arguments) for app
  compatibility investigations. Tracing is off by default.

The broker is intentionally not a general replacement for dbus-daemon yet.
It has no queued name ownership, activatable services, non-string argument
matches, credentials for FDs, monitors, eavesdrop, or cross-stub relay. Portal
implementations and Android mappings are outside this core.

## Test evidence

The host `broker-test` GLib/GDBus client (run with
`DBUS_SESSION_BUS_ADDRESS=unix:path=...`) opens two bus connections, requests
names, registers an object, verifies a unicast method round trip, subscribes
to a sender-filtered signal, checks `ListNames` and `GetNameOwner`, and checks
Flatpak and unlisted-name denial. The included server service can also be
called with `gdbus`. A `dbus-send`/`busctl` smoke can use the same bus address:

```sh
export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/matonos-bus.sock
gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
  --method org.freedesktop.DBus.ListNames
gdbus call --session --dest org.matonos.Test --object-path /org/matonos/Test \
  --method org.matonos.Test.Echo hello
dbus-send --address="$DBUS_SESSION_BUS_ADDRESS" --print-reply \
  --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.GetId
busctl --address="$DBUS_SESSION_BUS_ADDRESS" call org.freedesktop.DBus \
  /org/freedesktop/DBus org.freedesktop.DBus ListActivatableNames
```

### Real host application probes (2026-09-29)

Each app ran with `DBUS_SESSION_BUS_ADDRESS` set to its private broker socket;
the existing session bus was not used. Full call traces are retained under
`out/pc-logs/dbus-broker/`.

| App | Started? | Calls observed | Remaining missing services |
|---|---|---|---|
| GTK4 `gcr-viewer-gtk4` (GTK 4.14; opened `/etc/ssl/certs/ssl-cert-snakeoil.pem`) | Yes; stayed open until the 8-second timeout | `Hello`, `RequestName(org.gnome.GcrViewerGtk4, 4)`, `GetId`, `GetNameOwner` for portal Documents, GNOME/XFCE session managers, and portal Desktop; `AddMatch`/`RemoveMatch` for those owners and portal Inhibit signals/properties; `StartServiceByName` for `org.gtk.vfs.Daemon` and `org.freedesktop.portal.Desktop`; `org.a11y.Bus.GetAddress` | GVFS daemon, portals (Documents/Inhibit), accessibility bus, and session-manager services are absent. GTK warned about GVFS activation and the accessibility bus. |
| Qt6 Assistant 6.4.2 (GTK platform theme) | Yes; stayed open until the 8-second timeout | Two `Hello` connections; `StartServiceByName(org.gtk.vfs.Daemon)`; `AddMatch` for `NameOwnerChanged` with `arg0='org.a11y.Bus',arg1=''`; `NameHasOwner` for `org.a11y.Bus` and `com.canonical.AppMenu.Registrar` | GVFS is absent and activation remains denied. No portal call was needed to keep Assistant running. |
| Electron | Not run | No Electron executable or installed Electron app was present. The installed Hytale Flatpak launcher is a native ELF executable. | Recheck when an Electron app/runtime is available. |

The GTK probe needed one explicit `own org.gnome.GcrViewerGtk4` rule for its
observed `RequestName`; no `talk` rule was added. `StartServiceByName` and
`argN` match handling were added after observing real app calls. Service
activation remains denied by default; Qt needed no policy exception.

### Discord Flatpak host probe (2026-09-30)

Discord 1.0.160 was launched with `DBUS_SESSION_BUS_ADDRESS` set to an isolated
broker socket and an empty policy (all names denied); the host session bus was
not provided to the app. It opened an interactive main window and reached
`SESSION_ESTABLISHED`/`READY` using the cached account, so this run did not
exercise the login form. The broker logged 153 method calls in
`out/pc-logs/dbus-broker/discord-logged/broker-trace.log`.

Observed calls included repeated `Hello`, `AddMatch`/`RemoveMatch`,
`GetNameOwner`, `ListNames`, `ListActivatableNames`, `NameHasOwner`,
`StartServiceByName(org.freedesktop.Flatpak)`, `Properties.Get` on
`org.freedesktop.portal.Flatpak`, `org.freedesktop.portal.Documents.GetMountPoint`,
`org.a11y.Bus.GetAddress`, and `org.freedesktop.DBus.Peer.Ping`. Discord also
probed names used for StatusNotifier, ScreenSaver, AppMenu, Unity, IBus,
MPRIS, and Secret Service. Default-denied/absent services caused portal
version, accessibility bus, IBus, and system-bus warnings; they did not prevent
the main app window. No broker behavior or allowlist rule was added for
Discord. A complete desktop experience still needs app-scoped portal/accessibility,
IBus, Secret Service, and notification/status-notifier handling where those
features are desired; Flatpak service activation remains denied.

## MatonOS integration status

The broker is not integrated into the image and has no init service. For the
on-device bring-up test, `build-android.sh` builds a bionic x86_64 broker and
test client with GLib/GIO statically linked from the Flatpak spike's NDK build
inputs. Static GLib/GIO and their PCRE2/libffi dependencies are built under
`/mnt/data/aosp/out/matonos/flatpak-ndk/`; no upstream source tree is edited.
The binaries only need Android's `libz`, `libdl`, `libm`, and `libc` at run
time. This avoids depending on the current `/vendor/lib64` GLib/GIO from a
system-side process. A later image integration should wait for flatpak-spike's
system_ext GIO port.

Run the standalone NDK build with:

```sh
bash linux/dbus-broker/build-android.sh
```

It stages all generated libraries, objects, and binaries under `/mnt/data/aosp/out/`.
The test client binary runs the same name/routing/signal/deny checks as the
host harness.

The stub host must launch one broker for each app UID, create a socket in a
UID-owned private runtime directory, provide that app's policy file, set
`DBUS_SESSION_BUS_ADDRESS=unix:path=<socket>` for the app and all its child
processes, and stop/restart the broker with that app's lifecycle. App
processes must share the same UID as their broker to pass SO_PEERCRED; apps
must not share a socket or bus with another UID. No init service is needed.
Image product/module integration is deferred until the system_ext GIO port is
available.
