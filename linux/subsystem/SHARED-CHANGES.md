# Integration

Rounds 10+11 include the concrete integration edits in the v3-layout worktree.
No additional shared-file request or kernel change is needed. See
`out/pc-logs/agents/v2-retire-result.md` for coordinator validation and the
combined commit message. Historical handovers remain unchanged.

## Round 22: Development routing (required, outside this worktree's edit scope)

The compositor now implements `/org/freedesktop/Flatpak/Development` on the
existing authenticated Java backend. The broker already owns
`org.freedesktop.Flatpak` for SessionHelper, but its policy rejects Development
and its forwarder only accepts desktop portal paths. Thus **this feature cannot
be reached on the bus until the broker routing change is applied**.

A concrete, host-compiled change is ready at
`/mnt/data/aosp/out/pc-logs/v3-hostcmd/broker-routing.diff`. It changes only our
`linux/dbus-broker/broker.c` and `portals.c`: allow Development and its standard
Properties/Introspectable calls on this reserved name, allow the same object
through the internal unique name with the existing talk check, and forward that
exact path to Java. Keep SystemHelper, other Flatpak paths and name ownership
restrictions denied. ClientClosed notifications and SCM_RIGHTS forwarding
already work. No AOSP, bridge, SELinux or kernel change is proposed.

The task explicitly limits source edits to compositor and compat-proxy; no
broker source was changed without an expanded scope. Package both the routing
change and the Java/loader changes together before device validation.

## Downloads folder consent: blocked on Android storage scope

The requested Downloads symlink cannot be made usable by a stub through an
ordinary runtime storage permission on the Android 17 target. Android scoped
storage limits direct path access; the stock direct-path grant is the special
`MANAGE_EXTERNAL_STORAGE` access surfaced in Settings, which grants broad
shared-storage access rather than Download-only access. That broader grant
needs an explicit design decision before it is added to generated stubs.

No device-tree sepolicy change is proposed for now. AOSP's storage FUSE/AppOps
path governs access to `/storage/emulated/<user>/Download`; adding an SELinux
allow without a chosen permission flow would not grant it. Keep Downloads
private until the access scope is approved. Evidence and the Firefox/Chrome
fresh-image test matrix are in
`/mnt/data/aosp/out/pc-logs/agents/linux-downloads.report.md`.
