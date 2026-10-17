# Dead ends — explored, rejected, do not re-explore

These designs were built far enough to learn from and then dropped by the
project owner. The code is preserved only as tags, not branches.

## 1. Running Linux apps inside the stub's ART process ("in-process v3")

- **Tag:** `dead-end/v3-in-process` (`dcf4212`). Stages 1–3 had been merged
  into `main` and were removed again on 2026-10-17.
- **What it was:** a resident JNI runner that loaded Linux images into the
  stub app's zygote-created ART process, adopted Linux threads onto bionic
  threads, replaced images in userspace instead of `execve`, owned Linux fds,
  VMAs and exit state, and would have started every forked Linux child as a
  same-app `android:process` with an eager memory-copy fork emulation.
- **Why rejected (2026-10-17):** with the Wayland compositor, presenter and
  Xwayland in the stub's Android process, Linux apps need no ART at all.
  Running them as plain native processes under the compat proxy gives native
  `fork` (kernel copy-on-write), native `execve`, ordinary child processes and
  crash isolation from ART. The only thing in-process bought was a direct
  per-window Android `Surface`, and the zero-copy dma-buf → AHB →
  SurfaceControl path costs roughly 20–50 µs per frame instead.
- **Current model:** stub main process = ART + activities + compositor +
  Xwayland; Linux apps = native processes (`V3Launcher`, ProcessBuilder).

## 2. Private AHB socket transport for Vulkan presentation (GPU stage D2)

- **Tag:** `dead-end/ahb-wsi-transport` (`dd0fded`).
- **What it was:** a private `maton_ahb_manager_v1` Wayland protocol that sent
  Android-allocated AHardwareBuffers to a Linux process over a socketpair
  (`AHardwareBuffer_sendHandleToUnixSocket`), with release tracking, for a
  Vulkan swapchain without a per-window Android `Surface`.
- **Why rejected:** under the native-process model GPU clients present
  through standard `linux-dmabuf` (wrapped as AHBs by `dmabuf_import.c`, zero
  copy) with explicit-sync fences. No client needs the private transport.

## Related rejected direction

- **Direct per-window Android `Surface`/swapchain in the Linux process:** needs
  a Binder handle to the stub in a plain native process; there is no clean
  bootstrap without platform services (system bridge is off-limits for the
  subsystem). Dropped together with in-process v3.
