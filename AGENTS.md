# Linux subsystem app — agent instructions

Shared by all agents (Claude, codex, opencode) working in this repository.

## Priorities (project owner)

When goals conflict: **Google-certifiable Android > performance > compatibility > simplicity.**

## The compatibility layer is loud, never hidden

Do not disguise the compat proxy/layer as native Linux. Make it detectable and self-describing: say so in logs,
expose an environment marker with version, name synthesised /proc content and rewritten anonymous pages
([anon:matonos-…]) honestly, and make errors mention when the layer was involved. A per-app D-Bus portal for reading
layer details and setting per-app knobs (rewrite on/off, logging) is welcome.

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
The session bus stays our in-stub broker for now (no per-app bus daemon process). Elsewhere, prefer upstream code
over our own where it does the job (less code to maintain).

Future direction (keep designs compatible with it): Android app intents may be exposed over an app's D-Bus, and
cross-app messaging runs app ↔ its bus ↔ binder ↔ other app's bus ↔ app, bridged by the in-stub broker. Cross-app
policy belongs at the binder hop (Android permissions/intent rules), not in a D-Bus name filter.

Upstream Flatpak's own services are a different case and are used: `flatpak-portal`
(`org.freedesktop.portal.Flatpak`) and `flatpak-session-helper` (`org.freedesktop.Flatpak`), built from our pinned
Flatpak tree and run per stub.
