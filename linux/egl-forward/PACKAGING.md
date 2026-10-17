# Host APK packaging and VM acceptance

The subsystem host APK and launcher now include the EGL forwarder:

`app/build.gradle` builds from the pinned Khronos registries and libglvnd ABI
headers, then packages the library and manifest as x86_64 JNI plus an APK
asset. `V3Launcher` copies these into each launch work directory, maps
`/run/maton/egl`, and selects the vendor for every Flatpak launch through both
environment layers.

The host APK built and installed on the assigned VM copy. Its ZIP alignment,
APK signature, and native dependency closure passed. The VM store could not
install a test app: SELinux enforcing denied creation of its private DNS
socket, so the generated stub/launcher path and GL output remain unverified.
The README records the device denial and required acceptance steps. The JSON
already points at `/run/maton/egl/libEGL_maton.so`; the DSO is the Linux
libglvnd vendor adapter, not a packaged Android EGL implementation.
