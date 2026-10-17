# EGL/GLES forwarder — offscreen step

This directory implements a freestanding x86_64 libglvnd EGL vendor named
`libEGL_maton.so`. It forwards EGL 1.5 and GLES 2.0–3.2 calls to the resident
Android EGL/GLES libraries while switching to their bionic TLS, then restores
the Linux thread's TLS. The TLS backend and raw-syscall allocator are included
from `linux/vulkan-forward`; this directory does not fork that runtime.

The vendor accepts the GLVND ABI 0.2 handshake and exports only `__egl_Main`.
The JSON selects this vendor, and `eglGetPlatformDisplay` maps
`EGL_PLATFORM_SURFACELESS_MESA` to Android's surfaceless platform where
available, with a default-display fallback. The forwarder adds the MESA
surfaceless token to Android's EGL extension list. Pbuffers and GLES calls
are forwarded. EGL window/pixmap surfaces, client-buffer pbuffers, and EGL
images fail closed because Linux native handles cannot cross into Android.
The GLES debug-message callback entry point is also a no-op: invoking a Linux
callback from an Android driver thread would require a callback bridge outside
this offscreen step.
Desktop OpenGL is accepted only when Android reports it in
`EGL_CLIENT_APIS`; otherwise `eglBindAPI(EGL_OPENGL_API)` fails through stock
EGL. There is no Zink fallback in this step.

Each registry-generated EGL/GLES thunk caches the Android entry point, enters
resident TLS, makes one typed call, and restores Linux TLS. Unknown
`eglGetProcAddress` names return NULL. The adapter does not forward Linux
allocation callbacks or native window handles to Android.

## Build and host checks

Build outputs and downloaded dependencies stay outside the repository. The
Khronos XML files and libglvnd ABI headers are commit- and SHA-256-pinned in
`pins.json`.

```sh
OUT=/absolute/path/out/egl-forward
DEPS=/absolute/path/out/egl-forward/deps
NDK=/path/to/android-ndk-r30
bash linux/egl-forward/fetch-tools.sh "$DEPS"
CC=clang-21 READELF=llvm-readelf-21 \
  bash linux/egl-forward/build.sh "$OUT" "$DEPS" "$NDK"
CC=clang-21 bash linux/egl-forward/tests/run-host.sh \
  "$OUT/tests" "$OUT" "$DEPS"
```

The build checks zero `DT_NEEDED`, a single dynamic export (`__egl_Main`), and
compile-only arm64/riscv64 backends. `run-host.sh` checks the GLVND handshake,
builds the offscreen triangle program, and measures the shared TLS switch pair.
The offscreen program needs to run inside the adopted Android EGL bridge; host
Mesa's GLVND vendor ABI is not the Android library interface and is not a
substitute runtime.

Current host verification: the DSO builds with zero dependencies and one
export; arm64 and riscv64 stub compilations pass; the ABI handshake passes; the
offscreen harness compiles. The local FSGSBASE switch-pair measurement is
32.4 ns over 1,000,000 pairs. This is a host measurement of the shared
switch-to-Android/switch-back code path with the adoption lookup. Calls from
desktop apps that do not select this EGL vendor pay 0 ns. The end-to-end EGL
overhead and triangle readback are not verified yet.

## Stub launch integration still required

This task is scoped to `linux/egl-forward/`; it does not modify the shared host
APK or `V3Launcher`. `PACKAGING.md` records the exact follow-up needed to add
the DSO and JSON asset to the host APK, map them at `/run/maton/egl`, and set
`__EGL_VENDOR_LIBRARY_FILENAMES` for Flatpak launches. Until that is wired and
built, the Flathub `eglinfo` VM acceptance test cannot run.

## Next step

Step 2 can add Wayland surfaces using the existing Vulkan forwarding
`wsi_wayland.c` and the agreed `maton_ahb_buffer_v1` path. It must preserve the
Android TLS boundary and use AHB-backed EGLImages plus
`EGL_ANDROID_native_fence_sync`; none of those handles or extensions are
exposed here.
