# Vulkan Wayland WSI

Status on `vk-wayland-wsi`: **surface stage implemented; swapchain and
presentation are not implemented**. The surface API remains prototype-only.
A device probe verified that this VM's resident llvmpipe can export an Android
Vulkan image as an `AHardwareBuffer` backed by a dma-buf; the ICD does not yet
allocate or export swapchain buffers. The prototype ICD now advertises
`VK_KHR_surface` and `VK_KHR_wayland_surface`, accepts them at instance
creation without forwarding them to Android, creates a surface wrapper around
the app's existing `wl_display`/`wl_surface`, and reports graphics queue
support, a single RGBA8 format and FIFO mode. Capabilities are provisional
limits (1–4096 extent, 2–4 images), not yet connected to compositor configure
or presentation feedback. `VK_KHR_swapchain` remains unadvertised and no image
allocation, dmabuf transport, acquire/release, or present path exists.
`wsi.[ch]` remains a host state-machine prototype. Commit `4984170` provides
the freestanding ELF resolver for the application's already-loaded
`libwayland-client.so.0`; this stage uses it to validate the supplied
`wl_surface` proxy. No private buffer transport or Android Surface path is
used. The VM remains reserved for another install test, so no APK install,
resize, screenshot, or frame-timing result is claimed.

## Required data and process path

Linux apps remain native processes. The app's own Wayland client owns the
`wl_display` and `wl_surface`; the ICD calls its loaded
`libwayland-client.so.0` on the Vulkan caller's Linux thread. It resolves
symbols by walking the ELF `DT_DEBUG` link map, with `/proc/self/maps` as the
fallback. Resolution uses raw syscalls and ELF parsing; the ICD remains
freestanding with no libc dependency. Wayland objects use a private event
queue and proxy wrappers so the ICD does not consume application events.

The swapchain's Android-allocated, exportable buffers must be sent to the
compositor as ordinary `zwp_linux_dmabuf_v1` buffers with the verified DRM
fourcc, modifier, stride and offset. The compositor imports those dma-bufs as
AHardwareBuffers and attaches them to SurfaceFlinger through
`ASurfaceTransaction_setBuffer`. GPU frames are never composited or read back.
No private AHB socket, private buffer transport, direct Android Surface per
window, or in-process ART path is part of this design.

The verified VM probe path is a Vulkan image with
`VkExternalMemoryImageCreateInfo.handleTypes =
VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID`, followed
by `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` and
`VkImageDrmFormatModifierExplicitCreateInfoEXT` using the driver's advertised
linear modifier and an explicit plane layout. Dedicated memory allocation
uses `VkExportMemoryAllocateInfo` for the same AHB handle type. After binding,
the resident driver's
`vkGetMemoryAndroidHardwareBufferANDROID` returned an AHB;
`AHardwareBuffer_getNativeHandle` exposed two fds, with fd 0 reported as the
buffer dma-buf and fd 1 as gralloc metadata. For a 256x256
`VK_FORMAT_R8G8B8A8_UNORM` image, `AHardwareBuffer_describe` reported Android
format `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM` and stride 256 pixels.
`vkGetImageDrmFormatModifierPropertiesEXT`, with
`VK_EXT_image_drm_format_modifier` enabled, reported modifier `0` (linear).
`vkGetImageSubresourceLayout` on this modifier-tiled image reported plane
offset 0 and row pitch 1024 bytes; the AHB stride (256 pixels × 4 bytes)
agreed. The format-properties query reported exactly one supported modifier,
linear, with one plane.
The compositor's existing format table maps Android RGBA_8888 to
`DRM_FORMAT_ABGR8888`; this is the fourcc to advertise for that byte order.
This validates the export metadata on the VM driver, but the buffer has not
yet been passed through the compositor mapper validator. Full linux-dmabuf
import acceptance remains unverified. The alternate
`VK_EXT_external_memory_dma_buf` path was enumerated by the driver but was not
used.

## Synchronization and presentation

The ICD must attach the real Android acquire `sync_file` to every Wayland
buffer commit using explicit synchronization. The compositor passes that fd
to the SurfaceFlinger transaction without a CPU wait. SurfaceFlinger's release
fence returns to the compositor; the compositor sends it to the ICD as the
buffer release fence. The ICD does not reuse a swapchain image until that
fence signals. Missing callbacks, failed allocations, failed event-source
registration, and shutdown must retain or retire the image rather than unlock
it early.

Support `linux-drm-syncobj-v1` only when a DRM render node and timeline
syncobj plus timeline-point-to-`sync_file` conversion are available. On devices
without that path, support a sync-file protocol. The compositor must return
frame callbacks from SurfaceFlinger transaction callbacks and provide
`wp_presentation` feedback from presentation fences. The current compositor
still sends frame-done from its headless output callback; this is not valid
display pacing or presentation feedback.

Advertise FIFO only until FIFO pacing and release handling are implemented.
Add MAILBOX and IMMEDIATE only after their queue/retirement semantics work.
Handle resize through `VK_ERROR_OUT_OF_DATE_KHR`, with generation-based image
retirement. Do not expose incremental-present or mutable-format until their
underlying damage and image-format/view paths are implemented.

## Remaining implementation and verification

- Add Wayland instance extensions, `vkCreateWaylandSurfaceKHR`, surface query
  commands, and Vulkan swapchain entrypoints without forwarding Linux pointers
  or callback chains into bionic.
- Allocate external buffers through Android Vulkan, export and validate their
  dma-buf metadata, create `wl_buffer`s, and preserve correct image layouts,
  visibility and queue ownership across the Android/Linux boundary.
- Implement compositor explicit-sync and presentation feedback through the
  existing SurfaceControl presenter and release-fence lifecycle.
- Test Vulkan WSI state and wire encoding on the host; then boot the VM and run
  pinned Vulkan-Tools 1.4.341 `vkcube --wsi wayland`, resize, compare FIFO and
  MAILBOX timing, run Zink EGL Wayland, and collect screenshots. The VM endpoint
  was not available for this run.
- Verify on hardware with each supported resident Vulkan driver and with no
  DRM node. Unsupported buffer layouts or synchronization features must stay
  unadvertised.

XCB/Xlib WSI and Zink GLX are outside this Wayland task. An XID cannot be
treated as a `wl_surface`; Xwayland DRI3/Present would require a separate
validated dma-buf and release-lifetime path.
