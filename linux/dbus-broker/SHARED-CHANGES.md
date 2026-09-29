# Deferred system_ext integration notes

No shared changes are needed for the current standalone NDK bring-up test.
Do not apply the items below until flatpak-spike completes its system_ext GIO
port and the broker is explicitly scheduled for image integration.

The broker cannot safely link the current `/vendor/lib64/libgio-2.0.so`
from a `system_ext` process. The Soong GLib package has no GIO module yet.
Please coordinate these changes with the `flatpak-spike` owner before adding
the broker module to an image:

1. In `linux/third_party/glib/Android.bp`, add the Bionic x86_64
   `libgio-2.0_matonos` shared module in `system_ext`, linked against that
   package's `libglib-2.0_matonos`, `libgobject-2.0_matonos`, and
   `libgmodule-2.0_matonos`. It must expose the generated and upstream GIO
   headers needed by consumers.
2. In this directory, add a Soong `cc_binary` named
   `matonos-dbus-broker`, `system_ext_specific: true`, linked to
   `libgio-2.0_matonos`. Build with `-D_GNU_SOURCE`, C11, and warnings as
   errors. Keep it without an init rc/service for this bring-up.
3. In the existing inherited product makefile `buildinfra/buildinfra.mk`,
   add a clearly marked `# dbus-broker:` block with
   `PRODUCT_PACKAGES += matonos-dbus-broker`. This is the current product
   include hook; do not edit `device.mk` for this integration.
4. Add a temporary system_ext test-client module/package, or another
   supported way to run the existing C test client as an ordinary app UID
   during the fresh-image check. Remove its product installation after the
   test, while retaining the host harness and source.

Reason: `tools/build-pipewire.sh` currently stages GLib/GIO in the vendor
partition. A system_ext binary depending on that private vendor copy crosses
the linker namespace boundary. The Soong GLib modules are already
system_ext-specific, but they do not yet provide GIO. Do not work around this
by copying vendor shared libraries into system_ext; keep one system-side
library set with a declared Soong dependency.
