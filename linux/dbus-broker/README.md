# Per-app JNI session bus

SessionBus loads `libmatonos-dbus-broker.so` into the signed stub process.
maton_broker_jni.c, broker.c/broker.h and portals.c provide private bus
routing and MBP1 forwarding for Android-integrated desktop portals in the
in-process JavaPortal. The upstream Flatpak portal runs as a separate
same-UID process in the resident compat-proxy filesystem and owns
`org.freedesktop.portal.Flatpak` directly. All accepted peers must have the
stub's UID. The socket is 0600 inside its mode-0700 app data runtime directory.

The standalone broker, example service, privileged bus-control registration,
second native listener, supervisor PID/pidfd state, Flatpak-version handshake
and seccomp-notify SysV supervisor are deleted. No system-UID session peer is
accepted. The reserved Flatpak portal name cannot be claimed by clients;
FileChooser, OpenURI and other Android-integrated desktop portal methods use
the internal JavaPortal backend. The Flatpak interface bypasses that backend
so the upstream process receives the call. The SessionHelper query continues
to report unavailable, as it did when JNI supplied no monitor path.

build-android.sh links only the JNI broker/core/portal sources against GLib.
The Makefile builds current portal-wire, inhibition and wildcard-policy tests.
Native output and dependencies stay under out/. Fresh-image runtime checks
remain for the coordinator; host tests do not prove enforcing device behavior.
