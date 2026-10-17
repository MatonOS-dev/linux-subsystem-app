# Host APK integration request

The task worktree is limited to `linux/egl-forward/`, so the shared host APK
and launcher were left untouched. Add these changes when the integration
owner can edit those files:

1. In `linux/subsystem/host/app/build.gradle`, mirror the Vulkan build task:
   invoke `linux/egl-forward/fetch-tools.sh` and `build.sh`, place
   `libEGL_maton.so` in generated `jniLibs/x86_64`, place `10_maton.json` in a
   generated asset directory, rewrite `library_path` to
   `/run/maton/egl/libEGL_maton.so`, and add the task to `preBuild`.
2. In `V3Launcher`, copy the x86_64 native library and asset into a per-launch
   `egl/` directory, map that directory to `/run/maton/egl`, and set
   `__EGL_VENDOR_LIBRARY_FILENAMES=/run/maton/egl/10_maton.json` in both the
   Flatpak command environment and the process environment. Keep this
   platform-wide for every stub launch.
3. Build and install the updated host APK, then on the freshly booted assigned
   VM launch a Flathub app providing `eglinfo`. Run the surfaceless platform,
   verify EGL vendor and GLES renderer strings against Android's EGL/GPU, and
   retain the app reference and output. Run
   `linux/egl-forward/tests/offscreen.c` through the same adopted launch path
   and verify the pbuffer triangle readback.

The provided `10_maton.json` already points at the required `/run/maton/egl`
library path. `libEGL_maton.so` is a native x86_64 library, not a packaged
Android EGL implementation.
