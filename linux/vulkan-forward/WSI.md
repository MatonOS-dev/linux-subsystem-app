# Vulkan Wayland WSI

Status on `vk-wayland-wsi`: production WSI is now compiled into the ICD. The
ICD resolves the app's already-loaded `libwayland-client.so.0` from its ELF
link map, exposes Wayland surface queries, allocates swapchain images through
the resident Android Vulkan driver, exports their dma-bufs, creates
`zwp_linux_dmabuf_v1` buffers on a private event queue, and submits
presentations with an explicit `sync_file` acquire fence. The compositor
passes that fd to `ASurfaceTransaction_setBuffer`; its existing SurfaceFlinger
release-fence tracking delays ordinary `wl_buffer.release` until reuse is
safe. Steps 1–5 have host tests and the native compositor compiles. **No VM
runtime result is available:** at the time of this run, `adb devices -l` had
no device at `localhost:5556`, and `VM-FREE` was absent. APK installation,
`vkcube`, resize, screenshot, Zink, and frame-time measurements remain
unverified.

## Production join-up

`build.sh` always compiles `wsi.c`, `wsi_wayland.c`, the ELF resolver, and
`wsi_vulkan.inc`; there is no prototype build switch or production stub.
Instance enumeration advertises `VK_KHR_surface` and
`VK_KHR_wayland_surface` only if the required Wayland-client symbols resolve.
Surface creation checks for the compositor's linux-dmabuf global. Device
enumeration advertises `VK_KHR_swapchain` only when the app enabled the
Wayland surface extension. Swapchain commands are intercepted by the ICD's
local device dispatch before generated Android dispatch; the extension is
stripped before forwarding `vkCreateDevice`.

The ICD uses the app's own Wayland connection and client library in-process.
It resolves `wl_proxy_marshal_flags`, queue/listener/wrapper operations, and
Wayland core interface objects through ELF dynamic tables without libc or
`dlsym`. WSI proxies use a private `wl_event_queue` and wrappers so the ICD
does not consume app events. The production DSO retains zero `DT_NEEDED` and
only the three loader exports.

Each swapchain image is an Android Vulkan image created with
`VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`, a dedicated exportable
`VK_ANDROID_external_memory_android_hardware_buffer` allocation, and the
resident driver's linear modifier. `vkGetMemoryAndroidHardwareBufferANDROID`
provides the AHB; native-handle fd 0 is passed to linux-dmabuf as the image
dma-buf. The ICD validates AHB RGBA8 format, modifier, plane offset, and row
pitch against the AHB stride, then sends DRM fourcc `DRM_FORMAT_ABGR8888`
(numeric `0x34324241`), one plane, modifier 0, and the measured stride/offset
to `zwp_linux_dmabuf_v1`. This is the selected export path; the enumerated
`VK_EXT_external_memory_dma_buf` alternative is not used. The resident-driver
probe established that path for one VM llvmpipe configuration, but the
compositor's mapper acceptance has not been verified in a running VM.

## Fence flow and limits

`vkQueuePresentKHR` submits the app's wait semaphores followed by a temporary
exportable Vulkan semaphore, exports its completion as a `sync_file`, then
sends the fd with the private `maton_sync_file_v1.set_acquire_fence` request
before `wl_surface.attach`, damage, and commit. The compositor associates the
fd with the committed `wlr_surface` and transfers it to
`ASurfaceTransaction_setBuffer`; there is no CPU fence wait. The SurfaceFlinger
release fence is monitored by the existing presenter and only then does
Wayland send `wl_buffer.release`. The ICD dispatches those events on its
private queue and reuses only released images. This release notification is
the reuse gate; this protocol version does not send the release fd itself to
the ICD.

Only FIFO is advertised. The current acquire loop dispatches private-queue
events and waits for released buffers, but it does not implement precise
finite timeout accounting or frame-callback pacing. MAILBOX and IMMEDIATE
are not advertised. Surface resize/configure is not yet propagated from the
compositor to the ICD, so generation-based `VK_ERROR_OUT_OF_DATE_KHR` on an
actual window resize is not verified or complete. Existing swapchain state
does reject mismatched extents at creation and checks its generation on
acquire/present.

The explicit-sync protocol is a minimal private sync-file path. Driver support
for exporting the semaphore fd has not been verified on the unavailable VM;
the ICD requires the app to enable `VK_KHR_external_semaphore_fd` and fails
presentation if the resident driver lacks the required functions. No
`linux-drm-syncobj-v1` path is implemented. The compositor creates the sync
global optionally; failure to create it does not block compositor startup,
and WSI surface creation fails if it is absent.

## Tests and follow-up

Host tests cover state transitions, rollback, command interception,
libwayland symbol resolution, private queue registry/buffer creation, and
sync-file request/attach/damage/commit wire encoding. The WSI production build
checks zero `DT_NEEDED` and cross-compiles shared code for aarch64 and
riscv64. The native `maton_compositor` target was built in the isolated
`vkwsi-out` directory. Exact commands and results are in the run report.

When a free VM is available, install the APK and run pinned Vulkan-Tools
1.4.341 `vkcube --wsi wayland` under the subsystem compositor; verify dmabuf
import, explicit semaphore-fd export, resize/out-of-date handling, screenshots,
and FIFO behavior. Also run a Zink EGL Wayland client. No frame-time numbers
are available yet. After VM checks, document user tests on supported real
hardware and unsupported/no-DRM fallbacks.

XCB/Xlib WSI and Zink GLX are outside this Wayland task. An XID cannot be
treated as a `wl_surface`; Xwayland DRI3/Present needs a separate validated
dma-buf and release-lifetime path.
