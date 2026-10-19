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

Host verification on 2026-10-17: pinned inputs fetched; the DSO built with zero
`DT_NEEDED`, one dynamic export, and arm64/riscv64 stub compilation; the GLVND
ABI test passed; the offscreen harness compiled. The measured TLS switch pair
was **31.2 ns** over 1,000,000 pairs on the host (FSGSBASE, including adoption
lookup). Each EGL/GLES call through this vendor pays one such pair; code paths
that do not select this EGL vendor pay 0 ns. The host triangle execution did
not pass: this harness's fake bridge opens the host GLVND EGL DSO, which does
not provide the resident Android EGL entry points expected by the forwarder.
No glibc or musl pbuffer readback pass is claimed.

## Host APK and VM status

The host APK packages EGL in the `org.freedesktop.Platform.GL.maton`
extension. The relative glvnd manifest and `libEGL_maton.so` are mounted with
the extension and selected by Flatpak's GL driver extension mechanism. See
[`linux/gl-maton/README.md`](../gl-maton/README.md) for build and VM status.
The socket policy is outside this EGL repository and needs resolution before
the requested real stub-launch acceptance can run. Do not treat APK install as
proof of EGL runtime behavior.

## Real-PC verification

1. Build and install the updated `org.matonos.linux` host APK with the standard
   app build pipeline, then boot a fresh MatonOS image.
2. Install a Flathub app that provides `eglinfo`, launch it from its generated
   stub, and run `eglinfo -p surfaceless`. Confirm the vendor and GLES renderer
   match the resident EGL/GPU. Record the full app ref and output.
3. Build `tests/offscreen.c` for glibc and musl, run both through the same
   stub launch environment, and confirm the 64x64 pbuffer triangle's red
   center pixel with `glReadPixels`.
4. On a machine without an Android desktop-GL API, confirm
   `eglBindAPI(EGL_OPENGL_API)` fails cleanly. Wayland and X11 windows belong
   to step 2; they are not step-1 success criteria.

## Next step

Step 2 can add Wayland surfaces using the existing Vulkan forwarding
`wsi_wayland.c` and the agreed `maton_ahb_buffer_v1` path. It must preserve the
Android TLS boundary and use AHB-backed EGLImages plus
`EGL_ANDROID_native_fence_sync`; none of those handles or extensions are
exposed here.
