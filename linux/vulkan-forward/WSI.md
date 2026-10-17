# D1: Linux WSI over resident Vulkan and Android presentation

Status: host contract prototype, **not usable Vulkan WSI**. Based on main
`db7177b`. No VM use, compositor/Java changes, AOSP changes, or packaging.
The existing loader dispatch still hides every WSI extension and entrypoint.
D2 must meet the gates below before enabling any of them.

## Observed pipeline and process boundaries

`PerAppRuntime.java` runs the compositor and Xwayland session in the stub's
own UID/runtime, not a privileged shared compositor service. `StubActivity`
passes its Surface through `PerAppRuntime.attach` to `NativeCompositor`.
The Linux payload is a **different child process**, despite sharing the UID.
Its resident bionic bridge is local to that child; it cannot dereference the
stub's `ANativeWindow *`, `AHardwareBuffer *`, `wl_resource *`, or Java objects.
Linux `wl_display *` / `wl_surface *` are client-side proxies, not server IDs
that may be dereferenced in the compositor.

Code inspected in `linux/subsystem/native/`:

- `compositor_core.c`: creates wlroots headless outputs and per-window scenes;
  `on_frame` calls `maton_presenter_present` and immediately sends Wayland
  frame-done using a monotonic timestamp. This is not Android latch feedback.
  Xwayland surfaces enter the same scene via `xwindow_show_surface`.
- `presenter.c`: one SurfaceControl per scene buffer; dmabuf buffers are
  imported to AHB and submitted without a copy; shm textures are copied to a
  four-buffer AHB pool. Crop, destination scale, z-order and opacity are applied
  per layer. Geometry currently uses `ANATIVEWINDOW_TRANSFORM_IDENTITY`.
- `android_buffer.c`: allocation and dmabuf export rely on validated cros-gralloc
  metadata, BGRA format 5; CPU staging/upload exists if that metadata is absent.
- `dmabuf_import.c`: deliberately restrictive single-plane, mapper-safe format
  and layout acceptance. A dma-buf fd alone is not a portable AHB description.
- `android_output.c`: obtains a local ANativeWindow from a Java Surface;
  passes acquire fence ownership to SurfaceTransaction; dynamically resolves
  `setBufferWithRelease`, locks the wlr_buffer until its returned fence signals,
  then dispatches unlock to the Wayland loop. On allocation/thread failure its
  present fallback polls with timeout zero and unlocks anyway. That is unsafe
  for reusable WSI images; D2 must fix it rather than inherit it. The no-release-
  callback fallback also cannot establish WSI buffer reuse safety.

## Options

| Concern | (a) AHB-backed images, wl_surface buffers | (b) Per-surface ANativeWindow | (c) Copy to compositor buffers |
| --- | --- | --- | --- |
| Ownership/transport | Stub allocates AHB; sends complete handle with fds to child; child imports a **local** AHB into resident Vulkan. Compositor retains its own reference. | Stub owns Surface/BufferQueue producer; child needs a reconstructed local Surface/ANativeWindow through a Binder-capable trusted runtime intermediary. Sending its pointer or one fd is insufficient. | Child owns ordinary Vulkan images plus staging; sends bounded shm or a copy-compatible dma-buf with dimensions/stride to compositor; stub owns final AHB. |
| Scene integration | Custom wl_buffer wraps the original AHB; presenter submits it in the existing scene, preserving popups/subsurfaces. Potential zero-copy. | Android WSI owns buffer queue; associate a separate child SurfaceControl/Surface for every Wayland surface, including popup geometry/visibility/stacking; avoid double presentation by the existing presenter. | Existing shm path already copies into AHB pool; GPU transfer/readback and upload add cost. |
| Synchronization | Render-done sync_file to Android transaction; final release sync_file back to child. Track buffer and present serial. | Resident Android swapchain supplies acquire/present ordering; still needs Wayland commit/frame/presentation feedback and Android window-lifetime coordination. | Wait render/readback completion before exposing shm; release shm after copy; retain final AHB until Android release. GPU-copy fallback needs the same external fence discipline as (a). |
| Modes/pacing | FIFO only initially, queue scheduled on display cadence, release-driven availability. Mailbox needs explicit superseded-frame retirement. | Query Android modes and intersect with Wayland scheduling guarantees; do not forward Android answers blindly. | FIFO only initially, bounded copy/presentation queue; skip work for hidden windows without falsely reporting presentation. |
| Resize/rotation | New extent generation and swapchain; old images retire asynchronously. wl_surface scale/transform/crop feed presenter geometry. | Reconfigure producer dimensions/transform; handle Surface replacement/abandonment and old swapchains. | Reallocate staging/pools per generation; explicit stride/scale/rotation, checked byte sizes. |
| Practical risk | Needs custom buffer protocol, synchronization and PRESENT-layout adaptation, but reuses the scene/presenter architecture. | New Surface transport and producer lifecycle; Binder is forbidden inside Flatpak payloads. A broker outside the sandbox would be needed; not a handle we can simply hand to the Linux process. | Easiest correctness fallback when mapping/readback works, but bandwidth/latency and format conversion; not a claim that arbitrary GPU images are CPU readable. |

Recommendation: **(a)** for direct Wayland, with a correctness-first CPU-wait
synchronization milestone, then explicit fence transport. Keep (c) as an
explicitly negotiated capability fallback, not a silent performance promise.
Do not implement (b) in D2: it replaces too much of a working presentation
pipeline and requires a producer/Binder bridge without giving payloads Binder.
No new kernel modules or framework patches are needed for (a).

AHB ownership is portable across Android processes through the public NDK
`AHardwareBuffer_sendHandleToUnixSocket` / `recvHandleFromUnixSocket` APIs.
The receiving **bionic helper in the payload process**, entered on resident TLS,
creates the local AHB object; the freestanding ICD never decodes gralloc
internals. Use a dedicated socket for this helper's handle exchange; do not
interleave its opaque serialization with Wayland messages. Wire messages carry
connection-local IDs, generation, sequence, dimensions and capability bits,
never pointers. Authenticate peer/session, cap live allocations/bytes/fds,
validate lengths/enums and reject stale IDs before touching wlroots. Linux
callbacks/Wayland calls execute on Linux TLS, resident driver/nativewindow calls
on bionic TLS, with callbacks returning only after restoring the original TLS.

## Proposed direct-Wayland contract (D2, not implemented wire ABI)

1. A private Wayland extension binds a surface resource belonging to that same
   `wl_client` and creates a transport token. The ICD must use a Linux-side
   helper on the application's actual `wl_display` connection, with a dedicated
   event queue. Opening a second connection and reusing a wl_surface numeric ID
   is invalid. Resolve Linux libwayland through a Linux helper/function table,
   never the Android bridge's dlopen namespace. No new libc DT_NEEDED in ICD.
2. Negotiate format/usage/extent/fence capabilities and maximum storage. Stub
   allocates AHBs with GPU_COLOR_OUTPUT and COMPOSER_OVERLAY (plus sampled-image
   if actually supported/needed), one layer and no CPU usage for the fast path.
   The child receives handles via the paired authenticated socket and binds
   each buffer token to a real custom wl_buffer resource. Server-side wlr_buffer
   holds its AHB and advertises the capabilities the renderer actually supports.
3. Query resident external-image-format support with the AHB handle type for
   the exact format/usage/flags. Query AHB format/properties; reject
   `VK_FORMAT_UNDEFINED`/external-format-only/YUV buffers as render targets.
   Describe and verify extent/layers/usage on the resident side. Create optimal
   2D images with `VkExternalMemoryImageCreateInfo`, import memory with
   `VkImportAndroidHardwareBufferInfoANDROID` plus dedicated-image allocation,
   choose a bit in AHB memoryTypeBits and use its allocationSize. Bind offset 0.
   No pre-bind vkGetImageMemoryRequirements assumption for imported AHBs.
   The driver holds its own AHB reference on successful allocation; each side
   separately releases its transport reference. Destroy image before memory.
4. Acquire waits for final release ownership, then signals the application's
   semaphore/fence using a queue operation or a valid imported external payload.
   Zero timeout returns NOT_READY; finite timeout TIMEOUT; infinite waits must
   wake on connection death/loss. Device loss must unblock all waiters.
5. Present consumes **all** wait semaphores on the specified unwrapped queue.
   Initially submit a wait operation/fence and wait off the Wayland main loop
   before committing the buffer, giving an explicit completed-render contract.
   Later export SYNC_FD from a bridge-owned semaphore; send/duplicate fds with
   clear ownership. Vulkan retains no implicit ordering merely because it
   imported an AHB. Queue-family foreign ownership transfers and external
   memory visibility must be implemented and validated, not just CPU ordering.
6. Attach/damage/commit the wl_buffer to the original wl_surface. Server passes
   the acquire sync_file into SurfaceTransaction, retaining the wlr_buffer.
   Final Android release fence is returned/awaited before client reuse. A
   wl_buffer.release means no compositor reader remains; it is not a display
   timestamp. Dropped/hidden/pending buffers need the same retirement rules.
7. Separate frame opportunity, presentation completion and release events.
   FIFO needs a bounded ordered queue paced by real display/vsync feedback;
   headless output's immediate frame-done is not sufficient. Keep display
   feedback monotonic and map it to wp_presentation if exposed. Android may hold
   several buffers; one stalled window must not block the compositor or boot.

### Image layouts are a mandatory ICD change

These are ordinary external images, not Android swapchain images. Passing
`VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` through the current generated thunks is not a
valid implementation. D2 must track the imported images and translate PRESENT
layouts to a resident-supported layout (initial target GENERAL), with correct
external visibility/ownership transitions, in legacy and synchronization2
barriers, render pass attachment initial/final layouts (including RenderPass2),
and relevant nested structures/aliases. Preserve application inputs; copying
and selective adaptation must precede the generated validators/forwarding.
Audit every selected registry command with VkImageLayout fields. Implement
acquire/present transitions and semaphore semantics together. Validate with
resident validation layers before claiming vkcube support. If driver ownership
or layout restrictions defeat this approach, report the blocker; do not mask it
with invalid barriers or framework patches.

### Explicit vs implicit sync

Preferred external fence ABI is sync_file, already accepted by Android's
SurfaceTransaction. `wp_linux_drm_syncobj_v1` is a **DRM timeline** protocol,
not a sync_file fd protocol. It additionally needs render-node syncobj timeline
capability and timeline-point <-> sync_file conversion (DRM ioctls), correct
per-commit acquire/release points and monotonic ordering. Enable it only after
those paths exist in compositor and child; do not advertise it on CPU renderers
or merely because Vulkan timeline semaphores exist. The private AHB protocol
can carry sync_file fences without advertising syncobj. For standard implicit
sync clients use proven dma-buf reservation fences (import/export sync_file
where supported); do not assume our Vulkan queue updates them. CPU-wait initial
mode avoids an acquire race but still waits final Android release and requires
external image visibility/ownership correctness. Never release early on timeout,
OOM, missing callback API or failed worker creation: quarantine/retire instead.

### Present modes, geometry and queries

D2 surface support = exact resident physical device + queue family + negotiated
AHB import/transport support. Cache per surface/device, not globally. Advertise
only format/color-space/usage combinations validated end-to-end; RGBA8 UNORM
and SRGB_NONLINEAR is the D1 mock's sole pair, **not measured hardware support**.
Never equate a linear format with sRGB encoding; add sRGB views/mutable-format
only after compatible AHB image-format-list validation. FIFO is mandatory;
IMMEDIATE/MAILBOX/FIFO_RELAXED stay absent until their scheduling exists.
Support KHR_surface support/capabilities/formats/modes with count/INCOMPLETE
semantics, and KHR_get_surface_capabilities2 / surface_formats2 for Mesa.
Swapchain image counts must reflect Android hold count plus producer capacity;
D1's 2..4 and 4096 limit are test fixture policy, not production answers.

Wayland apps choose buffer extent independently of xdg configure; production
capabilities normally use the variable-extent UINT32_MAX sentinel and negotiated
limits. D1 deliberately uses fixed mock extent/generation for deterministic
resize tests. Map scale, viewporter, transform and configure acknowledgement;
Android Activity rotation updates output geometry, not a raw pointer handoff.
Track Surface attach/detach epochs separately from buffer extent changes;
mark genuinely lost surfaces lost and retire old swapchains after resize without
waiting on the Wayland/UI thread. `oldSwapchain` creation must retire prior
acquisition while allowing already acquired/presented images to complete.

## XCB/Xlib and Xwayland

An XID is not a wl_surface token. Xwayland owns the Wayland connection and
content updates; creating a second Wayland surface from an X client would lose
window identity, popups, clipping and X Present events. Do not implement
vkCreateXcbSurfaceKHR/vkCreateXlibSurfaceKHR as a cast to the Wayland adapter.

Production route: Linux-side XCB DRI3/Present helper exports AHB storage as
validated dma-buf planes/strides/offsets/modifiers, creates X pixmaps, and submits
Present to the specified drawable. Xlib's Display needs a supported Xlib/XCB
adapter and event-queue ownership handling. Xwayland then attaches those buffers
to its own wl_surface and the compositor presents them as today. Present idle
notification alone is insufficient unless Xwayland's release tracks the final
Android reader; carry release through wlroots and back to DRI3/Present. Check
fd and modifier capabilities on both sides, including implicit synchronization.
Normal AHB handle-socket serialization is not a DRI3 pixmap handle.

This checkout exports dma-buf metadata only for known cros-gralloc handles and
its compositor accepts a narrow layout set. Generic AHB->DRI3 export and the
current Xwayland DRI3 acceleration path are **D2 feasibility gates**, not proven
by D1. Probe DRI3/Present and modifier import before enabling XCB/Xlib extension
advertisement. If they fail, keep X WSI hidden and report it; an explicit shm
copy route via X pixmaps/Present is a slower possible separate milestone.
No Xwayland patch series is introduced. Any necessary upstream-code change
requires its own authorized fork work, not edits hidden in this prototype.

## Zink presentation requirements

Surfaceless Zink from C proves shader/device forwarding only. Build the pinned
Mesa with EGL Wayland and Zink's Vulkan WSI path (Kopper), libwayland and the
needed EGL window/xdg protocols. Its EGL Wayland window ultimately needs
VK_KHR_surface, VK_KHR_wayland_surface and device VK_KHR_swapchain through our
ICD, working acquire/present, required surface queries and correct formats.
Audit the pinned Mesa runtime for required optional extensions; the stage-C
profile audit identified incremental_present and swapchain_mutable_format.
Incremental damage can be accepted as a hint with correct full updates; mutable
format requires image-format-list/views and cannot be claimed as a no-op.
Do not simply add both to extensions.txt. Keep absent until implemented or
Mesa is verified to select a supported path. GLX/Kopper additionally needs the
XCB/Xlib route above; EGL Wayland success does not prove GLX success.

## What the D1 prototype actually does

`wsi.[ch]` is a plain-C internal surface/swapchain model with capability,
format, present-mode and image enumeration, allocate/import rollback,
FREE -> ACQUIRED -> QUEUED -> FREE ownership, serial-checked release and
resize/loss handling. `mv_wsi_import_ahb` constructs real Vulkan AHB import
structures through injected **raw resident** function pointers. It queries
AHB properties, validates a color format, creates an external image, chooses
memoryTypeBits, uses a dedicated import allocation, binds and rolls back on
failure. The caller must prevalidate external-image-format support and AHB
extent/usage and enter/leave resident TLS around this helper. No actual Android
AHB allocation/import executes on this host.

`MATON_VK_WSI_PROTOTYPE=1` links the model as hidden code into the ICD DSO.
The default build excludes it. Even the enabled build exposes **no WSI** via
ProcAddr/extensions. Host tests call the internal API directly. This build
switch is the D1 safety gate; there is no runtime enable switch to accidentally
make an incomplete driver visible. D2 wires loader wrappers only after the
above gates and negotiates runtime capability before advertisement.

`tests/wsi-test.c` uses a separate child process endpoint over SOCK_SEQPACKET
for PRESENT/RELEASE with buffer/session tokens and serials. It is a host-only
native-endian fixed record, not a proposed deployed protocol. Mock AHBs/images
are sentinels, and Vulkan callbacks inspect the actual create/import chains.
No pointer crosses the socket. The mock has no pixels, fds, compositor clock,
Wayland connection, GPU execution, fences or driver validation. Acquire is
nonblocking and has no Vulkan semaphore/fence parameters; present requires
render already complete; destroy returns NOT_READY for queued buffers. These
are internal prototype contracts, **not implementations of the Vulkan commands**.
Production allocator callbacks, locks, timeouts, retirement and oldSwapchain
are deferred to D2. The model rejects unsupported flags/pNext/modes/usage.

Reproduce (all outputs outside checkout, no VM):

```sh
bash linux/vulkan-forward/tests/run.sh "$OUT/default" "$VULKAN_HEADERS"
bash linux/vulkan-forward/tests/run-wsi.sh "$OUT/prototype" "$VULKAN_HEADERS"
```

The WSI test uses ASan+UBSan, validates short enumerations, partial import
cleanup, image identity, exhausted acquire/backpressure, wrong/duplicate/stale
release rejection, in-flight teardown refusal, resize/loss, dedicated import
fields and bind failure rollback. Both default and enabled DSOs retain zero
DT_NEEDED/undefined symbols and exactly three ICD exports; enabled WSI code
also compiles freestanding for arm64 and riscv64 (runtime backends still stubs).

## Exact D2 work list and acceptance gates

C / resident runtime / ICD:

1. `compositor_core.c`: private per-client AHB buffer protocol, session-bound
   transport pairing, capability negotiation, quotas, disconnect cleanup,
   per-commit sync state and buffer retirement; event-loop dispatch only.
   Keep Xwayland's ordinary scene association and probe DRI3/Present separately.
2. `android_buffer.[ch]` / `dmabuf_import.[ch]`: add a buffer wrapper for owned
   AHBs without serializing cros-gralloc structs. Expose a typed AHB lookup to
   presenter/renderer; keep strict dmabuf validation. Advertise only supported
   buffer capabilities. Preserve shm and existing dmabuf fallbacks.
3. `presenter.c`: recognize custom AHB buffers (including client-buffer source
   wrappers), consume each commit's acquire fence, retain per-present sequence,
   preserve scene crop/z/opacity, apply buffer transforms, bound FIFO queue.
4. `android_output.[ch]`: final release/fence ownership for every supported API
   path; fix early unlock error paths, replace unbounded detached waiter growth
   with bounded poll/event dispatch, quarantine unresolved readers, emit latch/
   presentation feedback distinct from release. Never wait on UI/Wayland loop.
5. Payload resident helper + launcher bootstrap: local nativewindow AHB receive/
   release functions, raw resident Vulkan capability/proc adapters, transport fd
   lifetime across sandbox launch, versioned Linux helper callbacks for Wayland
   and XCB. Keep binder inaccessible and CPU syscalls under arch/<arch>.
6. `icd.c`, `device.inc`, generator/validators: loader-facing surface/swapchain
   wrappers and per-enabled-extension ProcAddr, physical-query adapters,
   instance-extension filtering before Android create, internal device extension
   enablement for AHB/sync-file without exposing Android pointers to Linux apps,
   selective PRESENT layout/ownership adaptation, wait semaphore consumption,
   acquire signal/timeout semantics, app allocation callbacks and concurrency,
   oldSwapchain retirement and surface/device/transport-loss behavior. Do not
   forward Linux Wayland/Xlib extensions into Android vkCreateInstance.
7. Linux XCB/Xlib helper and packaging: verified AHB->DRI3 metadata/export and
   X Present release integration, or leave both extensions hidden with a gap.
   Add Mesa EGL Wayland/Kopper dependencies only after the direct route works.

Java (no new privileged API or Binder rights in the payload):

- `PerAppRuntime.java`: own WSI transport lifecycle alongside the Wayland session,
  publish only its private session endpoint/capability to the trusted launch
  path, revoke on close, coordinate native feedback with attached window epochs.
- `stub/StubService.java` / launch runtime: propagate versioned inherited fd or
  authenticated endpoint to the child through the existing trusted launch flow;
  do not accept arbitrary fds/paths from generated stubs. Stub identity remains
  the source of session authorization. No AHB parcel per frame in Java.
- `stub/StubActivity.java`: preserve attach/resize/detach/stopped callbacks; carry
  an epoch if native lifetime tracking requires one, so Surface replacement
  cannot deliver old releases to a new window. Handle repeated activity recreation.
- `runtime/NativeCompositor.java` and JNI glue: add lifecycle/feedback methods
  only where existing nativeStart/nativeAttach/nativeResize/nativeDetach cannot
  convey the epoch/transport setup. Surface stays in the stub; no Java Surface
  marshalling into the Linux payload for option (a). WindowActivity demo follows
  the same lifecycle. No framework/bridge permission expansion is required.

VM acceptance (D2 only; none run in D1):

1. Fresh APK/runtime, existing offscreen glibc+musl suites and surfaceless Zink
   regressions first. Verify actual resident AHB import, external format support,
   fd ownership and validation-layer output on lavapipe; record unsupported
   capabilities rather than fake them.
2. Unmodified Vulkan-Tools Wayland vkcube: visible animated frames, FIFO pacing,
   checksum/screenshot evidence, two windows, popup overlap, hidden/restored
   Activity, no premature reuse with delayed acquire/release fences. Repeat
   rapid resize/minimize/restore/rotation and Surface destruction/recreation.
3. Mesa Zink EGL Wayland gears or equivalent animated EGL window: log GL vendor/
   renderer/version, show swaps/animation, resize and multiple windows. Test
   core, compatibility and GLES contexts; failure must not silently use swrast.
4. Conditional X route: XCB and Xlib vkcube, then Zink GLX gears. Verify the actual
   Xwayland window, DRI3/Present events, popup stacking and resize; otherwise keep
   X extensions hidden and report the gate failure.
5. Fault injection: delayed/lost release, queue saturation, OOM, failed import,
   disconnect, stale tokens, malformed/fd-heavy requests, stopped Activity,
   process death with queued images, oldSwapchain recreation, multiple threads
   and app allocators. Assert bounded memory/fds/threads and no other window or
   boot blockage. Measure frame cadence and end-to-end latency vs copy fallback.
6. Hardware later: Intel/AMD/Surface GPUs, gralloc format/usage permutations,
   non-linear layouts and explicit/implicit sync capabilities. QEMU alone does
   not prove zero-copy or generic hardware compatibility.

## Primary references

- [AHB NDK API and Unix-socket handle transport](https://developer.android.com/ndk/reference/group/a-hardware-buffer)
- [AHB import reference ownership](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportAndroidHardwareBufferInfoANDROID.html)
- [Vulkan memory specification: AHB dedicated import](https://docs.vulkan.org/spec/latest/chapters/memory.html)
- [Image layout rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageLayout.html)
- [Wayland syncobj protocol source](https://cgit.freedesktop.org/wayland/wayland-protocols/tree/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml)

The pinned v1.4.341 headers used by the tests remain the build ABI; linked
latest reference pages explain the contracts and are not a dependency update.
