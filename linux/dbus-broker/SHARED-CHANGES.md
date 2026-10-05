# Integration ownership

The broker is built with static GIO by `build-android.sh` and delivered only
in MatonWaylandHost.apk as `lib/x86_64/libmatonos-dbus-broker.so`. The host
Gradle build invokes the script; no Soong broker module, product package,
system_ext prebuilt, or broker file_contexts entry is needed.

Flatpak runs from the static APEX. The session bus and portals are moving
into the stub app via dbus-java, replacing the native portal. See README.md
for the system Flatpak version compatibility floor and the transitional
broker registration behavior.
