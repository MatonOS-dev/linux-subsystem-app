# D-Bus services over binder (bridge)

Status: spec, phase 1 in progress (2026-11-09).

## Goal

An ordinary Android app can provide a D-Bus service to every Linux app. The
first user is the system tray app (`org.kde.StatusNotifierWatcher`, plus a
home-screen widget), but the bridge itself is generic and knows nothing about
trays.

Direction (subsystem AGENTS.md): Linux app ↔ its private bus ↔ binder ↔ other
Android app. Cross-app policy sits at the binder hop (Android permissions and
caller UID), never in a D-Bus name filter.

## One bus per app

A Linux app's private bus is the only D-Bus it has. Android's "system bus" is binder.
Names that Flatpak would send to the host system bus (`--system-talk-name`) appear on
that same private bus. Core services (UPower, logind, NetworkManager, ...) stay internal
(broker/Java backends); bridged Android apps provide the rest. The launch environment sets `DBUS_SYSTEM_BUS_ADDRESS` to the private bus
socket (env-var only; no fallback for a hard-coded `/run/dbus/system_bus_socket`).

## Model

- A **provider** is an Android app that exports a bound service:
  - intent action `org.matonos.dbus.SERVICE`
  - `<meta-data android:name="org.matonos.dbus.NAMES" android:value="name1,name2"/>`:
    the well-known D-Bus names it provides
  - no bind permission: the provider sees the caller via `Binder.getCallingUid()`.
- Any installed app may provide names, except names that could leak app data. The
  broker refuses to bridge a denylist (logged loudly): `org.freedesktop.secrets`,
  `org.gnome.keyring*`, `org.kde.kwalletd*`, `org.freedesktop.portal.*`,
  `org.freedesktop.Flatpak*`, `org.freedesktop.DBus*`, and names the broker reserves.
- When a stub starts its session bus, it queries
  `PackageManager.queryIntentServices(org.matonos.dbus.SERVICE)`. Then, for each provider:
  1. It binds the service.
  2. It creates a `socketpair(AF_UNIX, SOCK_SEQPACKET)`.
  3. It hands one end to the provider: `IDBusService.attach(ParcelFileDescriptor)`. The
     provider learns the stub from `Binder.getCallingUid()`.
  4. It hands the other end to the broker: `NativeBroker.nativeAddBridge(fd, names[])`.
- Binder carries only discovery, authentication and the socket handoff. All D-Bus
  traffic flows directly over the socket between the broker and the provider; no Java
  pumping happens on the stub side.

## Wire

Each SEQPACKET datagram is exactly one D-Bus message in standard wire format
(`g_dbus_message_to_blob`). There is no extra framing and no SASL handshake. Unix fds (the `h` type)
travel in the same `sendmsg`/`recvmsg` call as `SCM_RIGHTS` ancillary data, in the message's
fd-list order, exactly as on a real D-Bus Unix socket. `UNIX_FDS` in the header must match
the number of fds received; a mismatch drops the message with a loud log (dbus-daemon
behaviour). An fd that SELinux refuses to pass fails the send loudly. Messages larger
than the socket's max datagram size fail loudly. There is no made-up cap.

## Broker side: a bridge is a virtual client

`broker_add_bridge(Broker*, int fd, const char* const* names, GError**)` adds a
`Client` with no `GDBusConnection`:

- **Unique name and names:** it gets a fresh unique name (`:1.N`, same counter) and owns
  each listed name. `NameOwnerChanged` and `NameAcquired` fire as they do for a real client.
  Edge cases follow stock dbus-daemon semantics. A name that is already owned is not stolen:
  the bridge queues for it (like `RequestName` without `REPLACE_EXISTING`), logs that, and
  takes the name over when the current owner goes away.
- **Rule:** wherever the bridge hits an edge case, it does what stock dbus-daemon does.
- **Routing to the bridge:** everything routed to this client (method calls, replies,
  directed signals, matched broadcast signals) goes through the existing `send_raw()`.
  That function branches on the client kind and writes a datagram to the fd.
- **Routing from the bridge:** datagrams read from the fd (GLib fd source on the broker
  main context) enter the same routing as `route_message()` does for a real client:
  - method calls to any name, including the bus driver (`AddMatch`, `GetNameOwner`, …)
  - replies matched through `pending`
  - signals, broadcast or directed

  The sender is always overwritten with the bridge's unique name.
- **Connection loss:** EOF or an error on the fd is handled like `client_closed()`. The
  bridge's names are released, pending calls get errors, and the fd is closed.
- **Refactor needed:** the bus-driver handling (`bus_method_call`) currently takes a
  `GDBusConnection` and an invocation. Make it reachable for the virtual client: either
  refactor it to work on a `GDBusMessage` and return a reply message, or build the reply
  in the bridge path. Real clients must behave exactly as before.

## Known limits
- Caller credentials (`GetConnectionUnixUser` etc.) for a bridge peer return an error, never fake values.
- Datagram size is bounded by the socket buffer. Raise `SO_SNDBUF` on the pair; overflow fails loudly.

## Java side

- `IDBusService.aidl`, which providers implement:
  `void attach(in ParcelFileDescriptor socket);`
  It lives in the `shared` module so provider apps can depend on it.
- `DBusBridges` in the stub host code is owned by `SessionBus`:
  1. Discovery runs after `nativeStart` succeeds.
  2. It binds with `BIND_AUTO_CREATE`.
  3. On `onServiceConnected` it attaches and adds the bridge.
  4. On `onServiceDisconnected` it does nothing: the broker sees EOF.
  5. When the binding comes back, it re-attaches with a new socketpair.
  6. Unbinding happens in `close()`.

  Errors are logged loudly with the provider package name.
- `NativeBroker.nativeAddBridge(int fd, String[] names)` takes ownership of the fd.

## Test provider: `dbus-echo-test`

A tiny separate APK (its own gradle module) provides `org.matonos.test.Echo`:

- method `Echo(s) → s`
- method `Ping()`: emits the broadcast signal `org.matonos.test.Echo.Pinged(s caller_uid)`
- method `CallBack(s unique_name)`: calls `org.freedesktop.DBus.Peer.Ping` on that unique
  name and returns `"ok"` or the error name. This proves the reverse direction.

## What a stub imports and exports (from Flatpak metadata)

- **Imports:** only names matching the app's `talk` entries, from `[Session Bus Policy]` and
  `[System Bus Policy]` (wildcards like `org.mpris.MediaPlayer2.*` match). The stub queries
  binder providers and bridges each one that provides such a name. Names served by internal
  services (UPower, login1, portals, ...) are never imported. An app with `--socket=session-bus`
  (full access, no list) imports every provider.
- **Exports:** the app's `own` entries (minus the denylist), e.g. `com.valvesoftware.Steam`.

## Linux apps as providers (stub ↔ stub)

A stub is both consumer and provider. Example: Steam exports a "what is the user
playing" name; Discord's stub finds it over binder and imports it onto Discord's bus.

- Exported names = the app's Flatpak metadata `[Session Bus Policy] <name>=own` entries
  (minus the denylist). The stub manifest advertises them on its bridge service.
- The bridge service is that same `org.matonos.dbus.SERVICE` action, served by the stub.
  Its `attach(fd)` adds the fd to the stub's broker as a bridge that owns **no** names:
  calls arriving from the consumer route to the name's real owner on the provider's bus.
  The consumer side adds its end with the exported names. Socket = broker ↔ broker.
- No autostart of Linux apps: consumers bind with flags 0 (no `BIND_AUTO_CREATE`).
  The provider stub starts its bridge service only once its bus is up, so a consumer's
  pending binding connects when the provider app starts, and sees EOF when it exits.

## Phases

1. **Bridge, phase 1.** (1.5: fd passing via SCM_RIGHTS, plus VM enforcing test with memfd/pipe/file fds.)
   Phase 1 itself: Broker virtual client, JNI, AIDL, `DBusBridges`, echo provider,
   and tests. Not started: tray, widget.
2. **Tray app:** `StatusNotifierWatcher` + `StatusNotifierHost`, with DBusMenu read through
   the bridge.
3. **Widget + menu popup.**
4. **Stub as provider** (exported own-names, flags-0 binding).
5. **System names:** set `DBUS_SYSTEM_BUS_ADDRESS`, then providers for UPower/logind/... as needed.

Patching the taskbar to host the widget comes later.
