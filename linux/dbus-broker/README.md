# Per-stub D-Bus broker core

> **OBSOLETE (2026-10-07).** Superseded by the per-app runtime. Each stub is to
> host its own session bus, portal and compositor in its own process/UID
> (`docs/PER-APP-RUNTIME.md`); the session-bus/portal half is our own pure-Java
> D-Bus (`docs/OWN-DBUS.md`). This native GDBus broker and its `--host-session`
> forwarding are the transitional on-image mechanism and are to be **deleted**
> once the per-app runtime is validated on device. Do not add features here.
> The native `flatpak-portal` and launcher supervisor were already removed; the
> portal/supervisor text further down is kept only for the transition.

`matonos-dbus-broker` is a small per-stub session-bus broker. It uses GLib's
GDBus server transport and manually supplies the bus name registry, policy,
message routing, and signal match routing. Each app's processes connect to
the socket in their compositor session's delegated directory. Standalone
mode requires same-user authentication. `--host-session` runs as the
compositor app and authenticates system-UID (1000) native clients using both
SO_PEERCRED and GDBus peer credentials; it never enables anonymous auth.

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
daemon. The included `org.matonos.Test.Echo` hook is only a smoke-test service
and is available only when the broker is explicitly started with `--example`.

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
  --method org.matonos.Test.Echo hello # start broker with --example
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

The compositor owns one C broker process per Wayland session. The broker
binary is statically linked with GLib/GIO and its PCRE2/libffi dependencies;
it needs only Android's libz, libdl, libm and libc at runtime. It does not
start flatpak-portal or invoke bubblewrap. No init service is used.

Run the standalone NDK build with:

```sh
bash linux/dbus-broker/build-android.sh
```

It stages all generated libraries, objects, and binaries under `/mnt/data/aosp/out/`.
The test client binary runs the same name/routing/signal/deny checks as the
host harness.

`SessionBus` writes the session's generic `own <app-id>` and portal `talk`
policy from its validated ref, starts `--host-session`, waits for listener
readiness, and destroys/reaps it on host session shutdown. The broker has
PDEATHSIG and graceful SIGTERM cleanup. Legacy host-window launches also
use dedicated Wayland sessions. No application-specific rules are present.

linuxd delegates the validated Wayland directory on FD 198 and, when
available, the Xwayland directory on FD 199. The wrapper forks a native
supervisor, which connects `bus-control` (SOCK_SEQPACKET) through FD 198,
registers a gated portal child and its monitor path, and keeps both directory
capabilities private. The wrapper and portal close these FDs before exec.
Only the supervisor retains them. It creates a directory under linuxd's
private mode-0700 runtime parent and sends that directory FD with the
registration using SCM_RIGHTS. The mode-0777 child permits the compositor
broker to bind through the FD without opening the private native parent.
The broker validates the directory owner/type/mode and creates another
listener for the same bus at `session-bus-<supervisor>/bus`. Native clients
use that filesystem address; Flatpak binds/proxies only the socket into
its sandbox. Both listeners enforce identical peer credentials and policy.
The supervisor also provides Wayland/X11 relays in that directory, using
the same descriptor-preserving relay implementation as linuxd. This keeps
nested portal launches independent of a completed CLI's relays.

Proc FD paths remain private capabilities used only by native processes.
They are unsuitable upstream addresses for Flatpak's pivoted proxy helper,
and bwrap's source canonicalization cannot traverse the compositor's
private Android app-data bind mount. The native filesystem paths avoid both
failures. Control EOF removes the native bus listener; supervisor teardown
removes its display sockets and directory. Repeated ready launches reuse
the original supervisor and native socket paths.

The control listener requires a kernel-authenticated UID 1000 peer and is
protected by the compositor's private app-data parent and SELinux. Neither
the control socket nor its directory capability is mounted into an app
sandbox. The trusted wrapper registers only its own child while that child
is still blocked before exec of the fixed native flatpak-portal binary.
The broker pins the PID with pidfd_open. Only a D-Bus peer with the exact
registered PID and system UID, with a live pidfd, may RequestName for
org.freedesktop.portal.Flatpak, even if the policy says `own` that name.
Same UID, a claimed D-Bus sender, or PID reuse is insufficient. Each accepted
connection also belongs to one registration generation, so stale authenticated
connections cannot acquire authority from a later portal with a reused PID. Registration
EOF revokes authority and disconnects clients; the supervisor waits for
revocation before reaping, and pidfd validation also handles abrupt
supervisor failure. Broker/control EOF terminates the native portal. A
repeat launch reuses an already-ready live portal and the same supervisor.

### Java portals in the compositor host

The C broker retains authentication, per-app routing, default-deny policy,
Flatpak portal ownership and the native SessionHelper. Android portal logic
lives in the APK: Settings v2, Inhibit v3 and OpenURI v3. After Hello and
policy checks, calls to Desktop (or its broker unique name at desktop/request/
session paths) are forwarded unchanged except for their authenticated sender.
Unknown portal interfaces receive a Java UnknownMethod error. Standard Peer
methods remain native. No app-ID exceptions or bridge bypass is introduced.

SessionBus creates `portal-backend` beside `bus` before starting the broker.
This filesystem stream socket is mode 0600 in private compositor app data;
Java checks the compositor UID and a fresh 256-bit capability passed only in
the broker environment. The startup handshake is ASCII `MBP1`, then 64 ASCII
hex characters of that capability. Java acknowledges authentication with ASCII
`OKAY`; the broker waits up to two seconds before announcing bus readiness.
The broker clears it from its environment
after connecting. There is one backend connection per session, without
reconnection or stale request reuse.

Each subsequent frame contains two little-endian uint32 fields (D-Bus blob
length and descriptor count), followed by a complete standard D-Bus message.
The blob retains its own byte order. Limits are 1 MiB and 16 FDs. SCM_RIGHTS
is attached to the first header byte; partial sends do not resend rights.
The compositor reads and writes these frames with its own Java codec
(`DBusReader`/`DBusWriter`) over an Android `LocalSocket` (`LocalSocketChannel`).
It preserves this frame format, gathers `SCM_RIGHTS` as each frame arrives, and
checks the frame and D-Bus `UNIX_FDS` counts before exposing a message. Portal
semantics, state and caller checks live in `PortalBackend`; `PeerConnection`
dispatches method calls and signals. MBP1, SO_PEERCRED and the capability remain
the actual channel authentication, so no SASL is performed on this socket.
Received descriptors are passed to the portal as indices into the frame,
duplicated into ParcelFileDescriptor and closed after dispatch. Replies and
errors use the original serial and canonical destination; directed signals use
the same channel. Java sends no FDs back in this first protocol version.

The broker queues at most 256 forwards and retains at most 256 pending calls.
Forwards and channel IO run on the default GLib main context, outside the
GDBus filter worker. Writes have a one-second socket timeout; pending calls
have a 15-second deadline. Channel failure/deadline closes the transport and
fails outstanding calls. Host EOF clears Java requests and Android holds.
The internal ClientClosed signal carries the disconnected authenticated unique
name; Java releases just that caller's handles and file grants. Queued calls
recheck the live connection before dispatch, and queued state is retained
safely through broker destruction.

Settings reads Android night mode/configuration, API-34 contrast and the
Material You system accent resource where present. Read preserves its legacy
double variant; ReadOne returns one variant. ReadAll filters namespaces,
including empty patterns. Android owns title buttons, so button-layout is
`:`. Settings are sampled on each read; dynamic SettingChanged emission is a
follow-up. No hidden Android APIs are used.

Inhibit requests are Java objects scoped to the originating unique name.
Only suspend (4), idle (8), or both are accepted. Request.Close, client EOF,
channel EOF and host teardown release the aggregate partial wake lock and
activity keep-screen-on state. CreateMonitor/QueryEndResponse retain the
existing minimal running/screensaver-inactive monitor with private Session
handles. There is no logout/user-switch or live end-session source. The old
C implementation and byte-ACK `inhibit` socket are removed. The existing
Android wake-state forwarder still reports the wake lock through the bridge.

OpenURI launches ACTION_VIEW, preferring a verified session stub activity via
its Binder listener and falling back to host context with NEW_TASK. The ask
option uses Android's chooser. OpenFile/OpenDirectory consume SCM_RIGHTS FDs,
not caller paths, and publish bounded, random capability content URIs.
Read/write URI grants propagate to the stub and selected Android handler.
The public StorageManager proxy-FD API supplies read-only/read-write modes
and independent offsets via pread/pwrite on the received capability. It never
reopens a cross-UID native pathname; a read-only grant cannot inherit O_RDWR.
MIME is inferred from FD metadata; unknown types use application/octet-stream.
Only regular files/directories are accepted. Directory ACTION_VIEW depends
on an installed handler for directory content URIs; there is no DocumentsProvider
navigation/export or parent-directory revelation for regular-file FDs yet.
Failure is a portal response code 2; success means Android accepted the intent,
not that the receiving activity completed. File grants expire at caller/session
EOF. URI OpenURI rejects file/content/intent schemes so it cannot open arbitrary
host paths/providers on a sandbox caller's behalf.

`make test` builds the broker tests and portal-wire-test. `make check-java-portals`
runs standalone JVM unit tests, without Android/Gradle. `make check-portal-wire`
checks real GLib/Java messages in both byte orders, nested variants/dictionaries,
SCM_RIGHTS descriptors, fragmented reads and malformed frames without GSocket.
`make check-inhibit` uses the real broker accepted-connection path and the Java
backend over a test pipe adapter; status 77 means sandbox GIO sockets are denied.

`make check-portal-credentials` checks the actual RequestName credential
predicate: exact live PID and UID are required; an exited process's pidfd
cannot authorize a matching recycled PID. This check needs no GIO sockets.

### APK delivery and Flatpak compatibility

The host Gradle preBuild builds `build-android.sh` and packages its static-GIO
PIE executable as `lib/x86_64/libmatonos-dbus-broker.so`. Legacy JNI packaging
and `extractNativeLibs=true` let PackageManager extract it executable in
`ApplicationInfo.nativeLibraryDir`; SessionBus uses that absolute path.
Broker changes ship with `adb install -r MatonWaylandHost.apk`. The system
image no longer packages a broker executable. The native wrapper/portal
remain in the image; an image containing this registration metadata is
needed for the initial migration.

There is no independently numbered control protocol. The broker records
`PACKAGE_VERSION` from the image Flatpak build's generated `config.h`
(default `out/matonos/flatpak-ndk/flatpak/build/config.h`, override with
`MATON_FLATPAK_BUILD_CONFIG`). A missing/invalid build version fails the APK
build. At registration, the native supervisor runs the actual system
`matonos-flatpak --version` with a two-second limit. The broker accepts
numeric system Flatpak releases >= **1.14.10**, the explicit floor in
`flatpak-compat.h`, regardless of the version it was built against.
Future Flatpak incompatibilities require deliberate broker changes.

Older, malformed, or unknown versions are refused before the child gate is
released or a pidfd is registered. The control response carries
`org.matonos.DBus.Error.UnsupportedFlatpakVersion` and an explanation;
the supervisor logs it and terminates the gated child. D-Bus portal
`RequestName` also returns this error after rejected registration until a
valid registration succeeds. Broker logs include built, minimum and rejected
system versions. Malformed/legacy packet lengths fail closed.

Stock Android appdomain permissions allow nativeLibraryDir execution as
`apk_data_file` for installed APK updates and `system_file` for image apps,
without a domain transition. No broker exec label or additional grant is
needed. Execution from writable private app data is not used.
