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
