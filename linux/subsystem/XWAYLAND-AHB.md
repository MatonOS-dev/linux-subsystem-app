# Xwayland AHB backend

MatonOS is based on AOSP. X11 GL clients use the runtime's Mesa/Zink and
our Vulkan WSI; the Xwayland server uses Android EGL/GLES glamor. Neither hop
exports dma-buf planes or interprets a gralloc native handle. The compositor
and its existing `maton_ahb_buffer_manager_v1` protocol are unchanged.

## Buffer path and lifetime

1. The Vulkan WSI allocates RGBA8888 AHardwareBuffers and imports them into
   optimal-tiled Vulkan images with dedicated AHB external memory.
2. The WSI fills a Unix socketpair with
   `AHardwareBuffer_sendHandleToUnixSocket`. It sends the receiving socket by
   SCM_RIGHTS to the private Xwayland per-display import endpoint.
3. Xwayland receives the complete allocation and imports it through
   `eglGetNativeClientBufferANDROID`, `EGL_NATIVE_BUFFER_ANDROID` and an
   EGLImage-backed glamor texture. The imported pixmap is a normal X resource
   owned by the X client; disconnect and FreePixmap use stock cleanup.
4. After waiting for rendering completion, the client sends stock
   PresentPixmap. The private Present event queue uses Complete for FIFO
   scheduling and IdleNotify for image reuse. MAILBOX keeps stock Present's
   queued-pixmap replacement behavior.
5. Xwayland finishes its GLES work before getting the wl_buffer for either
   attachment path. It sends the AHB through the existing Wayland manager;
   wl_buffer.release retains upstream Xwayland's window-buffer tracking.

Server-created backing pixmaps also allocate RGBA8888 AHBs. Depth 24/32
ordinary TrueColor/DirectColor visuals are supported; other depths keep
upstream glamor's internal pixmaps and are not exported as window buffers.
RGBA is used for depth 24 too because the unchanged compositor's private
protocol accepts RGBA8888. No format/modifier negotiation is needed.

## Private local import contract

`XwaylandFiles` supplies `MATON_X11_AHB_DIR=<stub-runtime>/x11` to the server.
The endpoint is `<directory>/X<display-number>.ahb`, one AF_UNIX SOCK_SEQPACKET
listener per display. `V3Launcher` exports its matching absolute path as
`MATON_X11_AHB_SOCKET` to clients. This variable identifies the transport; it
is not a feature toggle. Both endpoints open the parent directory and use
`/proc/self/fd/<directory-fd>/<socket-name>` in sockaddr_un, so long package
runtime paths do not exceed the kernel's Unix socket address limit.

Each connection carries one packet of five native-endian uint32 words:
`width, height, AHB-format, drawable-XID, reserved-pixmap-XID`, plus one
SCM_RIGHTS fd for the already-filled AHB transport socket. Supported clients
are little-endian x86_64/i386. Xwayland replies with one uint32 pixmap XID,
or zero on import failure, and closes the connection. Reserving the XID with
xcb_generate_id makes ownership belong to the original X client. The server
checks the drawable through that client's normal X resource access checks.
Accepted import connections use the X server's fd event loop; screen teardown
closes outstanding connections and removes the endpoint. AHB transfer uses a
pre-filled socket so the server does not await producer work in its event loop.

The client retains its original AHB until swapchain destruction. The server
owns the received reference until the last pixmap reference is destroyed;
Wayland independently owns its transported reference. Import failures unwind
the received reference, image and pixmap. Producer fences are waited before
Present, and glFinish covers server writes before Wayland attachment because
the existing private protocol has no acquire-fence argument.

## Build and verification

Use a separate subsystem output and seed dependencies there. Copied Autotools
and Meson build directories contain absolute paths: clear/reconfigure those
build directories before invoking their install rules. The standalone
`build-xwayland.sh` applies `xwayland-ahb.patch` idempotently to pinned MIT
Xwayland 24.1.13. The patch selects the AHB backend, generates the existing
private protocol bindings, removes GBM gating and DRM registry binding, and
links the NDK's libandroid. Xwayland uses platform EGL/GLES dispatch; APK
packaging no longer installs the old Mesa/GBM forwarding libraries.

Host regression commands (outputs outside the repository):

```
bash linux/vulkan-forward/tests/run-wsi.sh "$OUT/wsi-tests" "$HEADERS"
bash linux/vulkan-forward/tests/run.sh "$OUT/core-tests" "$HEADERS"
```

The WSI test checks opaque RGBA AHB transport, a real kernel SCM_RIGHTS
round trip, request metadata and fd ownership, rejection cleanup, stock Present FIFO/Idle behavior, and teardown
without a final idle event. WSI state-machine tests cover rollback, release,
resize and backpressure; dependency tests verify the shared AHB requirement
array. Xwayland 24.1.13 compiled and linked successfully. Both signed APKs built in
`/home/hanro50/matonos/build/xwayland-ahb-out/`; the full build's host tests,
APK signature/alignment/dependency/bootstrap checks and x86-64-v2 ISA check
passed. Both Vulkan suites passed, including ASan/UBSan WSI tests and
x86_64/i386 ICD builds (three exports, zero undefined symbols/DT_NEEDED).

During initial dependency reuse, copied Autotools configurations reinstalled
some unchanged X11 dependencies into main-out before the process was stopped.
Subsequent configurations were rebuilt with isolated output paths. The
worktree's ignored, broken compat-proxy build symlink was also redirected to
`xwayland-ahb-out/proxy-build` so the existing placeholder fixture could run.

## On-device acceptance (pending; no VM or device tests in this task)

1. Install the signed subsystem APK through the normal package update path.
   Close/relaunch each test stub so it loads the new host and Xwayland.
2. In the app's normal launch environment, run `glxinfo -B` and `glxinfo -t`.
   Require working visual/context creation and a Zink hardware renderer.
3. Launch Steam normally. Verify the login/library UI, resize/repaint, then
   an animated GL view or game. A static launcher alone is insufficient.
4. Launch Abuse normally. Start gameplay, resize, and verify continuous
   rendering with correct colors and no black frames.
5. Close/relaunch both apps and repeat. Collect each stub's Xwayland, launch
   and crash logs. Check for successful glamor startup and no GBM/interface
   failure or software fallback. No device-tree or kernel changes are needed.

Compilation and host mocks cannot establish real driver AHB import, GLX
visual compatibility, frame correctness or GPU/compositor release timing;
those remain the acceptance checks above.
