# GLX to EGL client vendor

`libGLX_maton.so` is the libglvnd GLX vendor selected by
`__GLX_VENDOR_LIBRARY_NAME=maton`. The launcher sets that variable only when
Maton's GL extension is enabled. GLVND resolves the name to `libGLX_maton.so.0` from the extension library path;
GLX does not use the EGL ICD JSON manifest.

The vendor implements the libglvnd GLX ABI pinned in `pins.json`. GLX contexts
use the Maton EGL forwarder and a pbuffer sized to the X window. `glXSwapBuffers`
reads RGBA pixels from the EGL pbuffer and submits an XImage using XPutImage;
window resize replaces the pbuffer. This initial path prioritizes compatibility
and correctness. It does not use DRI3/Present or shared GPU buffers.

The current implementation is intentionally limited to one EGL config, direct
contexts, a single current context per thread, and window drawables. GLX visual
selection returns the default X visual; pixmap and pbuffer GLX drawables,
context sharing, and full GLX attribute negotiation remain unsupported. EGL
must expose desktop OpenGL; GLES-only devices do not provide this GLX vendor.

Build output and fetched headers belong outside the repository:

```sh
bash linux/glx-forward/fetch-tools.sh /absolute/out/glx-forward/deps
CC=clang-21 READELF=llvm-readelf-21 bash linux/glx-forward/build.sh \
  /absolute/out/glx-forward /absolute/out/glx-forward/deps /path/to/android-ndk-r30
```

The build checks the freestanding DSO has no `DT_NEEDED` entries and exports
only `__glx_Main`. Xlib, EGL and GL dispatch symbols are resolved from the
runtime process. EGL entry points are loaded from `libEGL.so.1` at runtime.

Host verification is `bash linux/glx-forward/tests/run-host.sh <out-dir>`;
the APK's GL extension packaging test also checks both GLX library names and
that the GLX JSON manifest is absent (GLX selection is through GLVND's
`__GLX_VENDOR_LIBRARY_NAME`).

VM status (2026-10-21): the vendor is selected in the Tux Racer stub and
reports GLX 1.4; the test obtains a visual and creates an EGL context, but
`glXMakeCurrent` does not return in the current VM. Extreme Tux Racer starts
its stub, but the captured screen remains black/garbled. Window context
binding and presentation are therefore still an open issue; this build is
not yet a working GLX implementation. Next debugging step is to trace GLVND's
make-current routing and the EGL pbuffer bind on the runtime's actual vendor
library.
