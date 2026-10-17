# D2b progress: dynamic transport storage and default-off global

D2b acceptance is **not met**. Loader-facing Wayland WSI, Vulkan synchronization
and display-paced FIFO remain unimplemented; no vkcube/Zink presentation claim.

The D2 transport policy caps have been removed: session resources and buffer
resources allocate individually; session references use checked `size_t` counts.
There are no per-session/process buffer or byte quotas and no session quota.
AHB allocation is attempted for nonzero supported-format extents. A failure
produces `failed(allocation)` without creating a wl_buffer. The compositor also
rejects extents above INT32_MAX after allocation: wlroots' buffer width/height
and scene geometry are signed integers. This is a representation requirement,
not a security policy, driver limit or Vulkan surface capability answer. There
is no 4096 limit and no row-byte multiplication/accounting to overflow.
Transport max fields now send zero, documented as no protocol policy limit;
the existing quota enum value is reserved, never emitted.

Release reservations are stable individually allocated linked nodes, reused
when free, with no count ceiling and no growing-array multiplication. Allocation
failure rejects the reservation before Android submission. Binder callbacks still
allocate nothing and never call wlroots. Unsafe fences/late shutdown readers
remain quarantined, retaining the original buffer until process exit. Free nodes
are cached at the process high-water mark; quarantine storage can grow if Android
keeps failing. No memory-pressure policy or claimed security bound is invented.

`maton_ahb_manager_v1` is **absent by default** from the actual compositor.
The coordinator may opt in with `adb shell setprop debug.matonos.ahb_wsi 1`
before starting a compositor instance; set it to `0` to disable for subsequent
instances. The property is read at startup, and release tracking/API availability
are still required. No ICD negotiation or Vulkan extension advertisement has
been added. The isolated protocol test intentionally creates its own global.

New test fixtures reserve 513 concurrent release readers and inject allocation
failure, and request 66 simultaneous buffers (including a 4097-pixel-wide buffer) plus 66 live sessions, crossing
every former count ceiling. The AHB tests require a device and are not a pixel
presentation test. See the D2b final report for executed verification.

The following is the historical D2 handover; its policy limits and statement
about default advertisement are superseded above. Its WSI implementation gaps
remain open.

---

# D2 partial: AHB transport and final-release prerequisites

**D2 is incomplete. No usable Vulkan WSI or Zink window presentation yet.**
The ICD still advertises no WSI extensions/entrypoints, even in its prototype
build. This change does not implement vkCreateWaylandSurfaceKHR, swapchain
commands, FIFO pacing, PRESENT layout adaptation or Vulkan acquire/present
semaphore semantics. No vkcube/gears screenshot or frame-rate claim is made.

## Implemented

`linux/subsystem/native/protocol/maton-ahb-v1.xml` is the private version-1
Wayland transport. A manager creates a session for a wl_surface on the SAME
wl_client connection. The server creates a nonblocking CLOEXEC SOCK_SEQPACKET
socketpair and sends one endpoint in the session's `transport` event. This
makes the existing authenticated Wayland connection the transport authority:
no filesystem socket endpoint, client-supplied transport, cross-connection
surface ID, Java fd propagation or native pointer crosses the wire.

The session advertises transport limits and supported AHB formats. Its
`create_buffer(request, width, height, format)` allocates RGBA8/BGRA8 AHBs with
COLOR_OUTPUT | SAMPLED_IMAGE | COMPOSER_OVERLAY usage. SAMPLED_IMAGE is required
by SurfaceControl even if the current compositor normally uses overlays.
Each nonzero request sequence must increase. Successful allocation sends one
complete nativewindow handle on the dedicated socket, then an `allocated`
event creating a server-ID wl_buffer. The receiver calls the NDK receive API
on resident bionic TLS, in event order; no gralloc structure is serialized by
our code. Failed requests create no client buffer. A failed send permanently
closes that session's transport, avoiding ambiguous handle/event ordering.
SIGPIPE is contained on the sending thread without replacing process handlers.

Limits are 4096 pixels per dimension, eight buffers / 64 MiB of row storage per
session, 64 buffers / 256 MiB process-wide, and 64 live sessions. These are
transport policy limits, **not measured VkSurfaceCapabilitiesKHR answers**.
Row-storage accounting includes actual AHB stride; it is not a portable measure
of hidden driver allocation overhead. Destroyed sessions keep accounting until
their retained buffers are actually destroyed. Destroying the associated surface
rejects further allocations. Client disconnect drops producers but never drops
a reader's lock. The buffer interface is registered with wlroots; typed AHB
lookup also unwraps wlr_client_buffer. The renderer retains a size-only texture,
and the existing generic presenter sends the original AHB to SurfaceControl,
preserving its existing crop, placement, opacity and z-order path. This change
does not add transform or display-feedback support.

Version 1 currently requires render completion AND external-memory ownership
release **before** wl_surface.commit. It carries no acquire fence. A future
ICD client must implement those transitions; buffer transport alone does not
make Vulkan writes visible. wl_buffer.release waits for all wlroots readers,
including Android's final release fence, and is not a presentation timestamp.
The XML is the authoritative implemented wire spec; WSI.md's larger D1 contract
remains the unfinished production WSI design.

Final-release tracking is a fixed 128-slot event-loop queue. Reservations occur
before submitting any buffer to Android. Binder callbacks only store the fence
and write an eventfd; they never allocate, wait, spawn threads or call wlroots.
The Wayland loop watches fences and unlocks only after successful signaling
(or -1, meaning no pending reader). Event-source/OOM/invalid-fence errors and
shutdown quarantine the retained reader rather than unlocking early. Stalled
readers consume bounded slots; saturation leaves existing layers in place and
retries. A callback arriving after shutdown closes its fd without dereferencing
wlroots. Quarantined slots remain occupied until process exit, including across
compositor restart. That is a deliberate safety cost, not leak-free recovery.
Missing API-36 release support refuses submissions; the app's minSdk is 36.
The transport global is omitted when release tracking/API setup is unavailable.

## Verification on localhost:5556

All outputs: `/home/hanro50/matonos/build/vk-wsi2-out/`.
Pinned Vulkan-Headers: v1.4.341. Reused compositor sources/caches were copied
from subsystem-crypto-out; no upstream dependency version changed.

- `device-probe.txt`: llvmpipe LLVM 21.1.8, Vulkan 1.4.354. RGBA8 and BGRA8
  optimal color/sampled AHB queries succeed; AHB import, foreign ownership,
  semaphore-fd and mutable-format extensions are present. Actual 64x64 RGBA8
  AHB allocation, properties query, dedicated Vulkan import and bind succeed.
  ASurfaceTransaction_setBufferWithRelease exists. This is no validation-layer
  or GPU rendering test.
- `native-build.txt`: full maton_compositor shared library built with NDK r30,
  API 36, x86_64, four jobs, standalone CMake (no AOSP build).
- `tests/buffer-release-test`: ASan+UBSan host tests pass delayed/signaled
  fences, loop-thread-only release, 128-slot backpressure, failed event-source
  registration, HUP quarantine, late callbacks and restart.
- `tests/release-vm.txt`: same tests pass natively under bionic on the VM.
  Their fences are synthetic eventfds/pipes, not real SurfaceFlinger fences.
- `protocol-test/vm.txt`: native bionic client/server, separate processes,
  eight complete AHB handle transfers, typed scene source, delayed reader,
  invalid extent, quota, closed transport/SIGPIPE and surface loss pass.
- `protocol-test/linux-vm.txt`: same protocol tests with a glibc Linux client,
  launched through the stage-C adoption spike, pass. Each transferred AHB is
  imported into an actual resident Vulkan image using D1 mv_wsi_import_ahb,
  then destroyed on resident TLS. Wayland dispatch stays on Linux TLS. The
  server is a purpose-built wlroots test peer using our actual protocol and
  renderer code; it is **not the stub compositor and has no Android Surface**.
- `default.txt`, `prototype.txt`: original ICD and D1 prototype regressions pass,
  including zero DT_NEEDED, zero undefined symbols, only three ICD exports,
  and compile-only arm64/riscv64 checks. No new CPU-specific code was added
  outside arch/<arch>; the Linux test includes the existing x86 backend.

Reproduce the transport builds (OUT and DEPS must be external):

```sh
bash linux/subsystem/native/tests/run-buffer-release.sh "$OUT/tests" "$DEPS/stage/x86_64/include"
bash linux/subsystem/native/tests/build-ahb-protocol-test.sh "$OUT/protocol-test" "$DEPS" "$HEADERS"
```

The second command builds native `ahb-protocol-test` and (when HEADERS is
provided) glibc `ahb-linux-client`. To select the Linux client on the VM, the
native test executable takes a launcher command: `ahb-protocol-test launcher
ld-linux-x86-64.so.2 --inhibit-cache --library-path linuxlib ahb-linux-client`.
It creates an inherited Wayland socketpair, starts the native test server in
its own child, and execs that launcher command with WAYLAND_SOCKET. The Linux
client terminates/reaps its own test-server child after its final roundtrip.
Runtime closure and launcher must be staged under /data/local/tmp/wsi and
removed after testing. No system files or app data are modified.

## Remaining gates and blockers

No external Vulkan capability blocker was found on this VM. The following
are **unimplemented work**, not an unsupported-driver diagnosis:

1. Versioned production Linux helper ABI for the application's own wl_display,
   event queue, timeouts, transport loss, release dispatch and resident helpers.
   The test directly exercises those crossings; it does not publish this ABI.
2. Loader-facing surface/swapchain commands and runtime-negotiated extensions;
   real device/surface capability queries; allocator callbacks, concurrency,
   oldSwapchain retirement and resize/out-of-date behavior.
3. PRESENT layout adaptation in every selected layout-bearing command, alias,
   renderpass and nested structure; queue-family external acquire/release,
   semaphore consumption/signaling and device-loss wakeup semantics.
4. Display/vsync-driven FIFO pacing and independent presentation feedback.
   Existing headless output frame-done is insufficient. Real Android fence
   callbacks, Activity detach/recreation and transform handling need validation.
5. Incremental-present hints and validated mutable image/view format lists for
   Zink, then pinned vkcube and EGL Wayland Zink animation/resize/reopen tests.

An APK update was not built/installed: this standalone build requires the
existing external signing keystore/password paths, which were not available
in the documented locations. These paths have been requested from the user.
This blocks installing this compositor change into the stub for device tests;
it does not explain or remove the unimplemented ICD work above.

No screenshots or frame timing were collected because no presentation was
implemented/tested. No binder access was granted, and no claim is made that
the root-launched transport test proves Flatpak sandbox compatibility.

D3 still needs verified AHB->DRI3 dma-buf export (planes, stride, modifier),
Xwayland DRI3/Present capability probing, and idle/fence notification tied to
the FINAL Android reader. XIDs cannot be cast to Wayland surfaces. XCB/Xlib WSI
and GLX remain hidden and unimplemented. No Xwayland or AOSP patch is proposed.

Real hardware follow-up, after the remaining implementation: repeat import,
validation-layer rendering, final-reader delayed fences and allocation-failure
checks on Intel/AMD/Surface GPUs; run Wayland vkcube and Zink EGL windows,
resize/rotate/minimize/restore, close/reopen, multiple windows/popups and
Activity replacement. Monitor FD/memory/thread bounds and reject silent software
fallback. Current VM transport tests cannot establish those results.
