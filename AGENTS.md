# Linux subsystem app — agent instructions

Shared by all agents (Claude, codex, opencode) working in this repository.

## Priorities (project owner)

When goals conflict: **Google-certifiable Android > performance > compatibility > simplicity.**

Hand-written x86_64/i386 assembly is welcome on hot paths when it measurably beats the compiler. Keep it under
`arch/<arch>/`, documented, with a C reference implementation and a test that compares both.

## File access: one EROFS image per package, mounted by matonos-erofsd (user, 2026-10-25)

Each app package carries ONE EROFS image; each runtime ID carries one image holding all its installed branches,
deduplicated by OSTree checksum. Stubs ask the image's system daemon `matonos-erofsd` (MatonOS image, frozen API) to
mount it; init performs the mount at `/mnt/matonos/<owner uid>/<n>` (only init may mount — domain.te neverallow).
The proxy maps package targets (`/app`, `/usr`, extension dirs) to the mount by prefix swap; everything after the swap
is native. Per-app mount namespaces / mountd APEX are a dead end (DEAD-ENDS.md). Future fallback when the daemon is
missing: extract the images into the stub's own data dir (not built yet).

## No job or thread caps (user, 2026-10-25)

Builds and tools use the whole CPU: default to `$(nproc)`, validate only that a job count is a positive integer.
Any job/thread/worker cap you find (fixed defaults like 4, "between 1 and N" checks, hard-coded `--workers=N`) is to be
removed when you touch that code, not worked around.

## The compatibility layer is loud, never hidden

Do not disguise the compat proxy/layer as native Linux. Make it detectable and self-describing: say so in logs,
expose an environment marker with version, name synthesised /proc content and rewritten anonymous pages
([anon:matonos-…]) honestly, and make errors mention when the layer was involved. A per-app D-Bus portal for reading
layer details and setting per-app knobs (rewrite on/off, logging) is welcome.

Crashes caused by the layer are reported as **MatonOS crashes**, not app crashes: attribute a fatal signal to the
layer when the fault is in proxy code, entries or stubs, inside a direct dispatch, at or near a patched site (rewrite
registry), or after a failed layer self-check. Other crashes are app crashes, annotated as running under the layer,
with the layer state (version, rewrite registry, recent proxied calls) attached.

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
