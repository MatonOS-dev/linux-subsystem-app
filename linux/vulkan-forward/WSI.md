# Vulkan Wayland WSI

Status on `vkcube`, 2026-10-19: **blocked before presentation; no animated
cube is claimed.** On the existing Venus VM (adb 127.0.0.1:5556), vkcube now
discovers `VK_KHR_surface` / `VK_KHR_wayland_surface`, selects Wayland and the
Virtio-GPU Venus RX 6600, creates its device and opens a black stub window.
The retained export path fails swapchain memory selection/allocation with
`VK_ERROR_OUT_OF_DEVICE_MEMORY`. A client-side `AHardwareBuffer_allocate`
experiment followed by the existing optimal-image import helper instead
reached Android's allocator, which aborted on a denied Binder ioctl:

```
Binder driver '/dev/binder' could not be opened. Error: 1 (Operation not permitted)
Binder ioctl to obtain version failed. Terminating.
Loader exited: 134
```

The denial is intentional: `linux/compat-proxy/arch/x86-filter.h` blocks
Binder ioctls before even the resident/raw-IP exemption; the host Binder
regression test verifies this. No SELinux or seccomp relaxation was made.
The aborting AHB allocation experiment was removed. Continuing requires a
design decision for **Binder-free client-owned AHB allocation**; do not
revive compositor-allocated buffer push or direct per-window Surface designs
(DEAD-ENDS.md). No compositor presentation, animation, resize or real-hardware
success is claimed.

Local evidence (outside the repository):
`/home/hanro50/matonos/build/vkcube-binder-blocker.log`,
`/home/hanro50/matonos/build/vkcube-progress2.png` (black Wayland window).
The VM was not rebooted; its VulkanInfo stub was never uninstalled.

Verified host suites: compat-proxy (including Binder denial and resident
exec), packs, standalone subsystem tests (also run by every signed APK build),
GL extension, Vulkan forwarding and WSI. The Vulkan suite's obsolete selector
benchmark and launcher dlerror callback type were repaired; adoption round
trips measured 25.3 / 25.4 / 29.5 ns for 1 thread / 64 live bindings / 64
concurrent threads. WSI retains zero DT_NEEDED, three exports, no undefined
symbols, and arm64/riscv64 cross-compilation. Signed full standalone APK and
bootstrap builds passed with four build jobs. These host results do not prove
runtime AHB allocation or presentation.

## Production join-up

`build.sh` always compiles `wsi.c`, `wsi_wayland.c`, the ELF resolver, and
`wsi_vulkan.inc`; there is no prototype build switch or production stub.
Instance enumeration advertises `VK_KHR_surface` and
`VK_KHR_wayland_surface` before the client loads Wayland. Client symbols are
resolved at surface creation, allowing applications that dlopen Wayland after
Vulkan extension discovery.
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
provides the AHB. When the compositor advertises the private
`maton_ahb_buffer_v1` global, the ICD creates a fresh `AF_UNIX/SOCK_SEQPACKET`
socketpair per image, calls stock `AHardwareBuffer_sendHandleToUnixSocket`
through its Android TLS bridge on one endpoint, and sends the peer endpoint in
`maton_ahb_buffer_manager_v1.create_buffer`. The compositor receives the same
AHB with `AHardwareBuffer_recvHandleFromUnixSocket`, checks its dimensions,
one layer and RGBA8 format, and retains it directly in the wlroots buffer.
No native-handle fd number or minigbm dma-buf import is involved in this
primary path. The compositor presents the received AHB directly to
SurfaceFlinger; frames remain zero-copy.

The standard fallback remains the verified
`VK_ANDROID_external_memory_android_hardware_buffer` → native-handle plane
fd → `zwp_linux_dmabuf_v1` path. It checks RGBA8 format, linear modifier, plane
offset and row pitch against the AHB stride, then uses DRM fourcc
`DRM_FORMAT_ABGR8888` (`0x34324241`), one plane and the measured stride/offset.
`VK_EXT_external_memory_dma_buf` is not used. The native-handle fallback was
probed on one VM llvmpipe configuration; runtime validation of the new AHB
transport still requires the VM.

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
are not advertised. On swapchain recreation, a compatible `oldSwapchain`
from the same Wayland surface is accepted. The new negotiated extent advances
the surface generation, causing acquire/present on the retired chain to
return `VK_ERROR_OUT_OF_DATE_KHR`; outstanding buffers can still drain before
destruction. Host tests cover this transition. Actual window-resize/configure
propagation through the compositor and app has not been verified in a VM.

The explicit-sync protocol is a minimal private sync-file path. Driver support
for exporting the semaphore fd has not been verified on the unavailable VM;
the ICD enables its private AHB, DRM-modifier and semaphore-fd dependencies
on the resident device when swapchain support is requested, deduplicating
application requests. Its resident instance uses at least Vulkan 1.1 for
those dependencies. Presentation fails if the driver lacks required functions. No
`linux-drm-syncobj-v1` path is implemented. The compositor creates the sync
global optionally; failure to create it does not block compositor startup,
and WSI surface creation fails if it is absent.

## Tests and follow-up

Host tests cover state transitions, rollback, command interception,
libwayland symbol resolution, private queue registry/buffer creation,
socketpair-backed AHB transport request encoding, and sync-file
request/attach/damage/commit wire encoding. The WSI production build
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
