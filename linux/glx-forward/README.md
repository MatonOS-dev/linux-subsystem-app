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
contexts, a single current context per thread, and window/pbuffer drawables. GLX configs
expose the server’s RGB8 TrueColor 24/32-bit visuals; legacy visual selection
returns the screen’s default visual. Pixmap GLX drawables and full GLX
attribute negotiation remain unsupported. EGL
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


## ANGLE initialization pbuffers and ES profiles (2026-10-29, round 57)

`day-r66/webhelper-linux.txt:62` reports “Could not create the initialization
pbuffer”; line 66 reports the missing `GLX_EXT_create_context_es_profile`.
The shim advertised window-only FBConfigs, returned zero from
`glXCreatePbuffer`, and rejected the ES context profile bit. These are GLX
capability gaps before ANGLE can initialize its dummy drawable.

FBConfigs now include `GLX_PBUFFER_BIT`. Pbuffer creation accepts the GLX
creation dimensions (`GLX_PBUFFER_WIDTH`/`HEIGHT`, distinct from the query
attributes `GLX_WIDTH`/`HEIGHT`) and creates a persistent EGL surface.
A server-allocated 1x1 pixmap supplies a collision-free XID, while all drawing
uses EGL; it is never queried as an X window or uploaded with XPutImage.
Reference counting keeps surfaces alive across context switches and deletion
while current. QueryDrawable reports the pbuffer dimensions before binding.
The two ES profile extensions are advertised; profile bit 4 selects
`EGL_OPENGL_ES_API`, preserving the requested version/debug attributes and
restoring the caller's bound EGL API. Actual version support comes from the
resident EGL implementation. Desktop GL retains its existing profile checks.
No application-specific overrides or software-renderer fallback were added.

The extended `tests/visual.c` uses Xvfb and the real Mesa EGL direct-table
adapter. It checks desktop GL and ES 2/3 green-pixel rendering on a 3x2
pbuffer, dimensions before make-current, persistence after switching to a
window and back, swap avoiding the X window path, and deletion while current.
The existing X11 visual/window red-pixel test remains included. The regression
fails against c0bf466 at pbuffer FBConfig selection and passes with this fix.
ES 1 is not exercised by this host's resident implementation.

Coordinator/device checks: rebuild/package both GLX vendor DSOs in GL.maton,
fresh-launch Steam, and confirm both initialization errors disappear and the
CEF GPU process stays alive. Verify a Steam window draws and resizes; repeat
Kate/Obsidian/Chrome checks. On Ryzen/RX6600, Surface Pro 3 and Haswell, repeat
the pbuffer/ES pixel regression through the installed GLX vendor on Xwayland.
Host Mesa/Xvfb coverage does not establish device rendering. No VM was run,
and no kernel or shared device-tree changes are requested. Steam's setup delay
is outside this change.

Validation logs are under `/mnt/data/aosp/out/pc-logs/steam-personality-r57/`.
GLX host regressions, x86_64/i386 freestanding GLX builds, Android loader
builds, Vulkan core/WSI suites, repository tests/run.sh and device preflight
pass. The complete compat-proxy suite was run but stops at the unchanged
marker-cache.py assertion (1,291 ancestry probes, expected fewer than 128).
A separate run of every remaining compat check passes (`compat-rest.log`);
this is not a claim that the full suite passes. No compat-proxy source was
changed by this round.
