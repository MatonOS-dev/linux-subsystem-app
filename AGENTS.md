# Linux subsystem app — agent instructions

Shared by all agents (Claude, codex, opencode) working in this repository.

## Do not add xdg-desktop-portal

`xdg-desktop-portal` (the `org.freedesktop.portal.*` frontend, and `xdg-permission-store` with it) is explicitly
not needed and must not be built, shipped or proposed.

Why: its job is mediating mutually hostile apps that share one session bus (caller identity, permission store,
per-app access decisions). In MatonOS every Linux app gets its own bus, and the Android app sandbox is the security
boundary, so that mediation is pointless. Our own thin portals (Settings, Inhibit, OpenURI, FileChooser → SAF, …)
translate requests to Android directly.

The only conceivable future reason to revisit this is letting Linux apps raise Android permission prompts through a
portal-style API. That would be our own thin portal mapped to Android's permission UI, still not
xdg-desktop-portal.

## No D-Bus name filtering

No `xdg-dbus-proxy` and no own/talk name filter: each app's bus exists only for that app, so filtering is pointless.
The session bus is a stock upstream bus daemon run inside the stub; prefer upstream code over our own where it does
the job (less code to maintain).

Upstream Flatpak's own services are a different case and are used: `flatpak-portal`
(`org.freedesktop.portal.Flatpak`) and `flatpak-session-helper` (`org.freedesktop.Flatpak`), built from our pinned
Flatpak tree and run per stub.
