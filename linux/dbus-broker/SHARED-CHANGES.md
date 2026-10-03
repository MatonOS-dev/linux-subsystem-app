# Integration ownership

The broker is built with static GIO by `build-android.sh` and delivered only
in MatonWaylandHost.apk as `lib/x86_64/libmatonos-dbus-broker.so`. The host
Gradle build invokes the script; no Soong broker module, product package,
system_ext prebuilt, or broker file_contexts entry is needed.

Flatpak and flatpak-portal remain native system components. See README.md
for the system Flatpak version compatibility floor and registration behavior.
