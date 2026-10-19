# Vulkan Wayland WSI

Status on `vkcube`, 2026-10-19: **blocked inside the VM Vulkan driver before
swapchain image import; no animated cube is claimed.** The Binder guard is
removed and the host regression exercises Binder ioctl pass-through from
payload text and libc. Android `AHardwareBuffer_allocate` returns success in
the resident path. The next call, `vkGetAndroidHardwareBufferPropertiesANDROID`,
aborts the process (Loader exited 134) before returning. Adding GPU sampled
usage to the AHB did not change the abort. The former Vulkan-export allocation
path also fails with `VK_ERROR_OUT_OF_DEVICE_MEMORY` and zero memory-type bits.

The evidence points to a VM/driver incompatibility at AHB import, but does not
identify why the driver aborts. Stop pending a decision or a VM whose Vulkan
driver can query/import an Android-allocated AHB. Do not use
compositor-allocated buffer push or direct per-window Surface designs
(DEAD-ENDS.md). No swapchain import, compositor presentation, animation,
resize, or real-hardware success is claimed.

Local runtime evidence (outside the repository):
`/mnt/data/aosp/out/pc-logs/vkcube/client-ahb-instrumented-logcat-3.txt`
shows the successful AHB allocator result and the last trace before the abort;
`client-ahb-full-last.txt` contains the corresponding system log. The first
attempt's screenshots are `client-ahb-1.png` and `client-ahb-2.png`; they show
the black window and then the installer desktop, not an animated cube. Signed
subsystem APK versionCode 496 was installed with `adb install -r`. The
VulkanInfo stub was never uninstalled, the VM was not rebooted, and the stub
was force-stopped after evidence collection.

Verified host suites: compat-proxy (including payload and resident Binder
pass-through), packs, standalone subsystem tests, GL extension, Vulkan
forwarding and WSI. Adoption round trips measured 25.3 / 25.4 / 29.5 ns for
1 thread / 64 live bindings / 64 concurrent threads. WSI retains zero
DT_NEEDED, three exports, no undefined symbols, and arm64/riscv64
cross-compilation. Signed standalone APK and bootstrap builds passed with
eight build jobs. Host results do not verify runtime AHB import or presentation.

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

Each swapchain image starts as a client-owned `AHardwareBuffer`, allocated by
the resident `libandroid.so` allocator with GPU color, sampled-image and
compositor usage. The ICD creates an optimal-tiled Vulkan image and imports the
AHB through `vkGetAndroidHardwareBufferPropertiesANDROID` plus dedicated
`VK_ANDROID_external_memory_android_hardware_buffer` memory. This is the
current primary path, but the Venus VM aborts inside the properties query
before image creation. When the compositor advertises the private
`maton_ahb_buffer_v1` global, the ICD creates a fresh `AF_UNIX/SOCK_SEQPACKET`
socketpair per image, calls stock `AHardwareBuffer_sendHandleToUnixSocket`
through its Android TLS bridge on one endpoint, and sends the peer endpoint in
`maton_ahb_buffer_manager_v1.create_buffer`. The compositor receives the same
AHB with `AHardwareBuffer_recvHandleFromUnixSocket`, checks its dimensions,
one layer and RGBA8 format, and retains it directly in the wlroots buffer.
No native-handle fd number or minigbm dma-buf import is involved in this
primary path. The compositor presents the received AHB directly to
SurfaceFlinger; frames remain zero-copy.

If client-owned allocation cannot be used, the retained export fallback is
`VK_ANDROID_external_memory_android_hardware_buffer` → native-handle plane
fd → `zwp_linux_dmabuf_v1` path. It checks RGBA8 format, linear modifier, plane
offset and row pitch against the AHB stride, then uses DRM fourcc
`DRM_FORMAT_ABGR8888` (`0x34324241`), one plane and the measured stride/offset.
`VK_EXT_external_memory_dma_buf` is not used. The native-handle fallback was
probed on one VM llvmpipe configuration. Neither path has reached presentation
on the current Venus VM.

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

## Native Android Venus AHB probe (2026-10-19)

Added `tests/ahb-native-probe.c` and built it directly with NDK
30.0.16248370's x86_64 API 36 clang. The probe dlopens
`/system/lib64/libvulkan.so`, creates a Vulkan 1.1 instance and graphics device
on `Virtio-GPU Venus (AMD Radeon RX 6600 (RADV NAVI23))`, enables
`VK_ANDROID_external_memory_android_hardware_buffer` and its advertised
external-memory dependencies, allocates 500x500 RGBA8 AHardwareBuffers, queries
AHB and RGBA8 format properties, and attempts a dedicated AHB import into a
VkImage. Select `AHB_USAGE_INDEX=0..2` to run one usage in a separate process:
0 = color output + sampled + composer overlay, 1 = color output + sampled,
2 = color output only.

All three `AHardwareBuffer_allocate` calls succeeded. The driver advertises the
AHB and dependency extensions; RGBA8 reports `linear=0x1dd83`,
`optimal=0x1dd83`. The first `vkGetAndroidHardwareBufferPropertiesANDROID`
call aborts for every tested usage, before returning properties; therefore image
creation/import is not reached. The native driver reproduces the vkcube failure.
The crash is SIGABRT in `/vendor/lib64/hw/vulkan.virtio.so`, with frames
`0x8ba7f, 0xd99f6, 0x9aca7, 0x9ae37, 0x11c1a8` (plus libc `abort+189`). The
plain tombstone contains no `Abort message` field/string, so there is no exact
abort text to report. Captured tombstones are `/data/tombstones/tombstone_07`
and `_08` on the VM (2026-10-19 17:02); crash log was retrieved with
`adb logcat -b crash -d`.

VM gralloc is `gralloc.minigbm_gbm_mesa.so` (`minigbm_gbm_mesa`); mapper is
`mapper.minigbm.so`, stable `mapper@5.0/minigbm`; allocator is
`android.hardware.graphics.allocator.IAllocator/default` AIDL version 3.
The Vulkan driver advertises Android AHB support but cannot complete the AHB
properties query here, so its Android AHB path is not usable on this VM. This
probe stops at the reproduced driver abort; no workaround or fix was attempted.

The native call sequence matches the forwarder's failing sequence: create the
Android Vulkan instance/device, allocate through `AHardwareBuffer_allocate`,
then call `vkGetAndroidHardwareBufferPropertiesANDROID` with
`VkAndroidHardwareBufferFormatPropertiesANDROID` chained for format details.
The forwarder invokes the same Android entry point after switching to Android
TLS and obtains it through its resident dispatch tables; this probe uses the
native process's direct device dispatch. No Vulkan result is returned by either
path. The forwarder has additional WSI allocation/image setup around this
sequence, but the failure occurs before those later calls can matter.
