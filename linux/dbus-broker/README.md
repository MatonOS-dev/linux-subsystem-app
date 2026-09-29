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
- `NameOwnerChanged`, `NameAcquired`, and `NameLost`; Peer `Ping` and
  `GetMachineId`; Introspectable `Introspect`.
- Default-deny `own`/`talk` checks; unicast request/reply/error routing and
  broadcast signal delivery for supported match fields (`type`, `sender`,
  `interface`, `member`, `path`, `destination`).
- All Flatpak and systemd bus names and PackageKit bus names are denied.

No GTK/Qt/Electron app has been exercised yet. The broker is intentionally
not a general replacement for dbus-daemon yet.
It has no queued name ownership, activatable services, argument match rules,
credentials for FDs, monitors, eavesdrop, activation, or cross-stub relay.
The first real GTK/Qt/Electron workload will determine which additions are
needed. Portal implementations and Android mappings are outside this core.

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

## Soong / image integration

The host Makefile is the only integration in this task. The eventual image
build needs a vendor `cc_binary` for the broker linked to the in-image GIO
and GLib modules, plus bundle init/config wiring for one socket and numeric
UID-scoped policy file per stub. It must remain outside the shared AOSP
module namespace until the linux/third_party GIO Soong modules and bundle
contract are ready. No AOSP patches or kernel changes are required here.
