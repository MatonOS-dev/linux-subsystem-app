# Vulkan Wayland WSI

Status on `vk-wayland-wsi`: **not implemented**. The ICD still exposes no
`VK_KHR_surface`, `VK_KHR_wayland_surface`, or `VK_KHR_swapchain` commands.
The host WSI model in `wsi.[ch]` is a test prototype, not a Vulkan
implementation. Commit `4984170` adds a freestanding ELF resolver for the
application's already-loaded `libwayland-client.so.0`; it does not create a
Wayland surface or swapchain. The configured VM at `localhost:5556` was
unavailable during this run, so no Vulkan app, resize, screenshot, or frame
timing result is claimed.

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

The exact Vulkan allocation and export sequence is still unimplemented and
must be documented when it is proven. Candidate Android-side allocation is
`VK_ANDROID_external_memory_android_hardware_buffer` followed by native-handle
dma-buf fd extraction; use `VK_EXT_external_memory_dma_buf` only if the
resident driver exposes and validates it. Validate each format/modifier with
the resident driver and the compositor's mapper metadata checks. The current
compositor only accepts mapper-verified, single-plane formats; it does not
prove that arbitrary Android Vulkan swapchain allocations are importable.

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
