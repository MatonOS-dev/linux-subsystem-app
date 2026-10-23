# EGL/GLES forwarder — offscreen step

This directory implements a freestanding x86_64 libglvnd EGL vendor named
`libEGL_maton.so`. It forwards EGL 1.5 and GLES 2.0–3.2 calls to the resident
Android EGL/GLES libraries while switching to their bionic TLS, then restores
the Linux thread's TLS. The TLS backend and raw-syscall allocator are included
from `linux/vulkan-forward`; this directory does not fork that runtime.
The same forwarder also builds as ELF32 with `-march=corei7`; its i386 backend
uses `int 0x80`, Android's `/system/lib` paths, and the adopted v2
`switch_in`/`switch_out` hooks. The generator stays architecture-neutral, so
its registry output and proc lookup map are shared by both builds.

The vendor accepts the GLVND ABI 0.2 handshake and exports only `__egl_Main`.
The JSON selects this vendor, and `eglGetPlatformDisplay` maps
`EGL_PLATFORM_SURFACELESS_MESA` to Android's surfaceless platform where
available, with a default-display fallback. The forwarder adds the MESA
surfaceless token to Android's EGL extension list. Pbuffers and GLES calls
are forwarded. Wayland window surfaces use the runtime's `wl_egl_window`
object and `wl_surface`; no libwayland link is added. The adapter uses a
resident Android pbuffer as the app's default framebuffer, then blits each
completed frame into a client-allocated `AHardwareBuffer` exposed to GLES
through an EGLImage. It sends that AHB to the compositor with
`maton_ahb_buffer_v1`, exports an `EGL_ANDROID_native_fence_sync` fence, and
presents through the shared `linux/wayland-client` module. That module also
backs Vulkan WSI and owns the Wayland symbol resolver, private event queue,
registry, frame callbacks, buffer releases, and sync-file requests. EGL reuses
the AHB only after its Wayland release arrives. Resize replaces the pbuffer
and AHB after the previous release; swap interval 0 skips frame-callback
waits and interval 1 waits for frame pacing. Buffer age is always zero.
Pixmap surfaces, client-buffer pbuffers, and EGL images remain unsupported.
The GLES debug-message callback entry point is also a no-op: invoking a Linux
callback from an Android driver thread would require a callback bridge outside
this offscreen step.
Desktop OpenGL support is probed against the resident EGL implementation with
`eglQueryAPI` / `eglBindAPI`, then the previous thread API binding is restored.
The probe also requires access to that same vendor EGL instance through
`android_load_sphal_library("libEGL_<ro.hardware.egl>.so", ...)` and its own
`eglGetProcAddress`; no second driver copy is loaded. The vendor client API
string includes `OpenGL` only after this probe succeeds. When the hardware EGL
library or desktop API is absent, desktop support is rejected cleanly while
the GLES path remains available.

The pinned Khronos `gl.xml` generates TLS-switching thunks for desktop GL 1.0
through 4.6 compatibility profile. EGL proc lookup routes GLES commands
through GLES thunks. The GLX direct table uses desktop thunks for the whole
desktop compatibility API, including names shared with GLES, so GLX calls
resolve against the desktop EGL driver's dispatch table. Desktop-only
commands also resolve from that driver, never AOSP's limited GLES proc
table. There is no Zink fallback in this step.

Each registry-generated EGL core and extension thunk caches the Android entry
point, enters resident TLS, makes one typed call, and restores Linux TLS.
Extension spellings and promoted aliases are generated from `egl.xml`; EGL
image, sync, device, platform-display, and native-fence commands pass through.
The extension string removes entries whose registered command signatures need
Android-only native handles, then probes every advertised extension command
with Android's `eglGetProcAddress` and removes extensions with missing
commands. Probe results are cached in the stub's `XDG_CACHE_HOME` as
`matonos-egl-extensions.cache`, keyed by `ro.build.fingerprint` and the
graphics backend/renderer/EGL/GLES/Vulkan properties. The launcher copies that
app-private cache path into the read-only GPU bootstrap, so processes of that
stub can share it. If it cannot read or write the cache, probing continues
in-process. Unknown `eglGetProcAddress` names return NULL. Linux allocation
callbacks and Linux window handles are not passed to Android.

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
bash linux/egl-forward/tests/egl-extensions.sh "$DEPS" "$OUT/extension-tests"
```

The build checks zero `DT_NEEDED`, the expected EGL exports, ELF class for each
x86 build, and compile-only arm64/riscv64 backends. Run
`tests/proc-aliases.sh "$DEPS" "$OUT/i386-proc"` to exercise proc lookup map
generation for i386; the checked-in generator emits the same API table for
both ELF classes. `run-host.sh` checks the GLVND handshake,
builds the offscreen and desktop triangle programs, and measures the shared
TLS switch pair. The desktop harness is built as `desktop-glibc` and, when
`MUSL_CC` or `musl-gcc` is available, `desktop-musl`. It dynamically loads
`libEGL.so.1` so both libc variants can use the same adopted Android EGL
bridge. `desktop.c` exercises a compatibility-profile pbuffer with
`glBegin` / `glEnd` and checks the red center pixel; invoke it with
`--expect-unsupported` on a vendor without desktop GL to verify clean
`eglBindAPI(EGL_OPENGL_API)` rejection.
The offscreen program needs to run inside the adopted Android EGL bridge; host
Mesa's GLVND vendor ABI is not the Android library interface and is not a
substitute runtime.

`egl-extensions.sh` checks the five requested extension spellings, the
generated Android-handle exclusion, token filtering, and cache-key invalidation.

Host verification on 2026-10-17: pinned inputs fetched; the DSO built with zero
`DT_NEEDED`, one dynamic export, and arm64/riscv64 stub compilation; the GLVND
ABI test passed; the offscreen harness compiled. The measured TLS switch pair
was **31.2 ns** over 1,000,000 pairs on the host (FSGSBASE, including adoption
lookup). Each EGL/GLES call through this vendor pays one such pair; code paths
that do not select this EGL vendor pay 0 ns. The host triangle execution did
not pass: this harness's fake bridge opens the host GLVND EGL DSO, which does
not provide the resident Android EGL entry points expected by the forwarder.
No glibc or musl pbuffer readback pass is claimed.

The i386 build additionally passes the ELF32/exports/no-dependency checks with
the shared proc-alias test. Runtime execution remains part of the 32-bit
Platform compatibility-extension test plan in `linux/gl-maton/README.md`.

## Host APK and VM status

The host APK packages EGL in the `org.freedesktop.Platform.GL.maton`
extension. The relative glvnd manifest and `libEGL_maton.so` are mounted with
the extension and selected by Flatpak's GL driver extension mechanism. See
[`linux/gl-maton/README.md`](../gl-maton/README.md) for build and VM status.
Do not treat APK install as proof of EGL runtime behavior.

## Real-PC verification

1. Build and install the updated `org.matonos.linux` host APK with the standard
   app build pipeline, then boot a fresh MatonOS image.
2. Install a Flathub app that provides `eglinfo`, launch it from its generated
   stub, and run `eglinfo -p surfaceless`. Confirm the vendor and GLES renderer
   match the resident EGL/GPU. Record the full app ref and output.
3. Run `tests/desktop.c` built for glibc and musl through the same stub launch
   environment. Confirm the 64x64 compatibility-profile pbuffer triangle's
   red center pixel with `glReadPixels`.
4. On a machine without a desktop-GL API, run either desktop harness with
   `--expect-unsupported` and confirm `eglBindAPI(EGL_OPENGL_API)` fails
   cleanly. GLES support should remain unchanged.

## Wayland window verification

**VM test pending** because the assigned VM is busy. Once free, launch the
subsystem compositor and run runtime `es2gears` or `eglgears` through a
Flatpak stub. Verify first presentation, resize, buffer release and reuse,
swap interval 0 and 1, and surface destruction. If neither app is present,
use a small EGL client that creates a `wl_egl_window`, draws a changing GLES
triangle, and resizes the window. Collect logcat and compositor logs; host
protocol tests do not establish runtime success.
