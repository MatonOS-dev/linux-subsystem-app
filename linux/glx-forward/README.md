# GLX to EGL client vendor

`libGLX_maton.so` is the libglvnd GLX vendor selected by
`__GLX_VENDOR_LIBRARY_NAME=maton`. The launcher sets that variable only when
Maton's GL extension is enabled. GLVND resolves the name to `libGLX_maton.so.0` from the extension library path;
GLX does not use the EGL ICD JSON manifest.
It is built for both x86_64 and ELF32 `corei7`; the i386 vendor loads the
matching i386 EGL forwarder and uses the same versioned direct-table ABI.

The vendor implements the libglvnd GLX ABI pinned in `pins.json`. It obtains a
private, versioned entry table from `libEGL_maton.so`; it must bypass GLVND's
`libEGL.so` front end because GLX already owns libGLdispatch's make-current
transition. Desktop GL calls use the Maton EGL vendor's TLS-aware GL thunks.
Each GLX window is rendered to an EGL pbuffer sized to the X window, then
`glXSwapBuffers` reads pixels and submits an XImage with XPutImage. Resize
replaces the pbuffer. This does not use DRI3/Present or shared GPU buffers.

The current implementation is intentionally limited to one EGL config, direct
contexts, a single current context per thread, and window drawables. GLX configs
expose the server’s RGB8 TrueColor 24/32-bit visuals; legacy visual selection
returns the screen’s default visual. Pixmap and pbuffer GLX drawables,
context sharing, and full GLX attribute negotiation remain unsupported. EGL
must expose desktop OpenGL; GLES-only devices do not provide this GLX vendor.

Build output and fetched headers belong outside the repository:

```sh
bash linux/glx-forward/fetch-tools.sh /absolute/out/glx-forward/deps
CC=clang-21 READELF=llvm-readelf-21 bash linux/glx-forward/build.sh \
  /absolute/out/glx-forward /absolute/out/glx-forward/deps /path/to/android-ndk-r30
```

The build checks both freestanding DSOs have no `DT_NEEDED` entries, verifies
ELF32 for i386, and exports only `__glx_Main`. Xlib symbols are resolved from the runtime process. EGL
entry points are loaded from the Maton GL extension's `libEGL_maton.so` at
runtime; the versioned direct table avoids re-entering libGLdispatch.

Host verification is `bash linux/glx-forward/tests/run-host.sh <out-dir>`;
the APK's GL extension packaging test also checks both GLX library names and
that the GLX JSON manifest is absent (GLX selection is through GLVND's
`__GLX_VENDOR_LIBRARY_NAME`).

VM status (2026-10-21, third investigation): context creation, binding and
pbuffer drawing pass (`GL 4.6 Compatibility Profile Mesa 26.2.3`, center
BGRA `0,0,255,255`, GL error 0). Swap readback and `glXWaitGL` now resolve
Maton's TLS-aware GL thunks explicitly rather than relying on front-end GL
symbol interposition. The host swap fixture checks thunk use, channel masks,
row orientation and upload.

Presentation is still **not accepted**: the triangle's XGetImage center was
`0x77b01a`, not red. A separate Xlib-only probe also fails: XFillRectangle
read back `0x40a3ac`, and XPutImage read back zero after allowing the window to
settle. An earlier XPutImage readback returned red once; it is not reliable.
The launcher screenshot, without a Linux window in the foreground, was also
black/corrupt. Thus this VM has a presentation/readback problem beyond GLX;
changing the shim's byte packing cannot by itself fix this baseline. Further
work needs to isolate native Xwayland/glamor, compositor and the VM graphics
stack before selecting a workaround. No software-renderer fallback was added.

Extreme Tux Racer no longer exits 159: strace located the fatal call in a new
resident pthread's `sigaltstack` (x86_64 syscall 131), while SIGSYS was blocked.
The proxy's early unconditional stack trap preceded its bionic exemption.
Resident bionic stack setup now follows that exemption; Linux payload stack
calls remain virtual. The game stayed alive for more than a minute with five
threads, but its screenshot is corrupt, so working rendering and a meaningful
frame time are not claimed. No existing stub was uninstalled or VM restarted.

## Real-PC checks

1. Build and install the updated subsystem APK, then launch an X11/GLX Flatpak
   such as Extreme Tux Racer from its generated stub.
2. Confirm `__GLX_VENDOR_LIBRARY_NAME=maton` is present in the app environment,
   `glXQueryVersion` reports 1.4, and the app can create and bind a context.
3. Run the GLX red-triangle readback harness. Its center pixel must be red
   both from `glReadPixels` on the pbuffer and `XGetImage` on the X window.
4. Capture the game window while it is rendering and check for stable frames;
   repeat after resizing the window.

The current QEMU run passes context creation, binding, pbuffer drawing and
game startup, but fails steps 3 and 4. First check plain Xlib drawing/readback
and the launcher screenshot as baselines. Do not treat the shim as complete
until presentation passes.

The frame-pacing syscalls use each process's Linux ABI: x86_64 `syscall`,
i386 `int 0x80` with the time32 `timespec` layout. Exercise both without a
GPU or libc with `bash tests/timing.sh /absolute/external/output`.


## Steam/ANGLE requested visuals (2026-10-29, round 56)

The exact “Invalid visual ID requested” error in day-r65/webhelper-linux.txt
(line 62, repeated) comes from ANGLE's **DisplayGLX** implementation, rather
than our EGL X11 platform. ANGLE enumerates GLX configs and compares
`GLX_VISUAL_ID` to `EGL_X11_VISUAL_ID_ANGLE`; no match means EGL initialization
fails. See [ANGLE DisplayGLX.cpp](https://chromium.googlesource.com/angle/angle.git/+/master/src/libANGLE/renderer/gl/glx/DisplayGLX.cpp).
Our single synthetic config always returned visual ID zero and visual type
zero, despite glXGetVisualFromFBConfig returning the default visual.

Configs now describe the real server TrueColor RGB8 visuals (depth 24/32),
with matching visual IDs, GLX_TRUE_COLOR type and screen. They share the
existing resident RGBA8/D24S8 pbuffer format. Config selection, visual lookup,
context creation and GLXWindow validation use the same config identity; the
actual server visual must match a created window. No new EGL presentation
backend, app rule, environment override or Android patch is needed.

Validation: tests/visual.c reproduces ANGLE's requested-visual loop on Xvfb,
checks both depths and visual types, creates a real EGL desktop context via
a host Mesa direct-table adapter, makes the selected X window current, swaps
and checks a red XGetImage pixel. It fails against main 7f6f936 at the zero
visual ID and passes with this change. The adapter substitutes only the
resident EGL/TLS boundary; this is not an Android/Xwayland rendering claim.
Existing GLX ABI/swap probes pass, as do x86_64/i386 Android-targeted GLX DSO
builds (no DT_NEEDED, only __glx_Main exported). Full compat-proxy (including
marker-cache), Vulkan core/WSI, EGL extension tests, Android loader builds
and device preflight pass. The older EGL run-host switch benchmark has an
existing mv_sys link failure; it is unrelated to GLX and is not changed here.

Coordinator: package the new GLX vendor and Vulkan ICD in GL.maton, fresh-launch
Steam and check that both GL/GLES ANGLE initialization pass, a CEF GPU process
stays alive and a window renders. Retest Kate/Obsidian/Chrome. Real PC checks
on Ryzen/RX6600, Surface Pro 3 and Haswell: repeat the visual/context/swap test
through the adopted runtime on Xwayland, then Steam resize/relaunch. No VM
was started. Steam rendering and its setup delay remain device-verification
items; no shared device-tree or kernel changes are requested.
