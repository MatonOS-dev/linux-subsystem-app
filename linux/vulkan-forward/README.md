# Vulkan forwarder — offscreen stages A–C

Freestanding Linux ICD forwarding to the resident Android Vulkan loader on
MatonOS (based on AOSP). Stage B passes unmodified `vulkaninfo --summary`, four
adopted-thread compute, and triangle readback on the existing lavapipe VM.
Stage C also passes dynamic musl compute/render and Mesa Zink surfaceless GL 4.6
core/compatibility plus GLES 3.2. See [STAGE-C.md](STAGE-C.md) for pins, exact
reproduction, capability audit, and limitations.
This experiment is not a Vulkan conformance or production-readiness claim.

## Dispatch and ownership

Only the three ICD entry points are exported; interface version 5 is negotiated.
The pinned Khronos v1.4.341 registry is SHA-256 verified before generation.
All 234 core 1.0–1.4 commands are covered. There are 283 canonical commands,
68 extension aliases (351 lookup names), 262 generated plain-C thunks, and 21
manual adapters. `coverage.txt` in the external output lists every name.
`extensions.txt` explicitly lists 71 device extensions for offscreen Zink
requirements and optional paths. Enumeration intersects this with the Android
driver's list and preserves revisions; all 71 are present on this VM. Instance
ICD extension/layer lists are empty. No WSI, surface, swapchain, external-handle,
or debug-callback command is forwarded. The Khronos loader independently exposes
its own four instance extensions, which appear in `vulkaninfo`.

Instance, physical-device, device, queue, and command-buffer wrappers begin with
`VK_LOADER_DATA`. Every call unwraps its receiver; non-dispatchable handles pass
through. Physical devices and queues retain identity across enumerations and
queue getters. Device/command-pool destruction and explicit command-buffer free
release child wrappers. Manual adapters rewrite device-group physical-device
chains, submit/submit2 command-buffer arrays, and secondary command-buffer arrays.
Command-buffer wrappers are allocated before calling Android so allocation failure
cannot leak newly allocated Android buffers.

Generated dispatch tables cache Android entry points per instance/device. Calls
use `switch_to_android()`, a typed Android call, then `switch_back()`. Alias
spellings fall back to Android extension entry points when a canonical pointer
is absent. ProcAddr returns only allowlisted names also found by Android.
Driver feature/property payloads are preserved; the instance API ceiling is 1.4.

No Linux allocation callback enters bionic. Android receives NULL allocators;
app `pAllocator` allocates/frees our dispatchable wrappers on app TLS, using the
instance/device allocator as appropriate. With no app allocator, each allocation
owns an mmap mapping; there is no fixed arena, thread cap, or libc allocator.
Temporary copies also use mmap. This deliberately does not make the app's
callbacks account for Android driver allocations.

## Chains and callbacks

The generator walks selected registry structures to validate input structures,
including nested structure arrays. Unknown input pNext types, callback-bearing
nodes, and unhandled embedded dispatchable handles fail closed with
`VK_ERROR_FEATURE_NOT_PRESENT` (void commands return without forwarding).
The whitelist covers the selected core and extension structures; it does not
promise future extension compatibility. Output query chains remain Android
query payloads; physical-device group outputs are specially rewritten.

Device-create chains are copied without mutating application memory.
`VkDeviceGroupDeviceCreateInfo` physical-device arrays are unwrapped in the copy.
Khronos `VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO` nodes contain Linux loader
bookkeeping/callbacks and are stripped: the Android loader supplies its own.
Device-memory-report callbacks and other unallowlisted nodes are rejected.
Instance-create pNext is stripped entirely: v1 forwards no instance extensions,
and Linux debug/direct-driver-loading callbacks must never reach bionic.

## libc-free bootstrap and architecture

There are **zero DT_NEEDED entries and zero undefined symbols**. Build flags are
`-ffreestanding -nostdlib -fno-stack-protector -fno-builtin -fvisibility=hidden`;
the linker also enforces `--no-undefined`. No libc code or TLS is linked/bundled.
Our memcpy/memset/memcmp/strcmp and raw-syscall allocator are self-contained.

The launcher publishes a read-only anonymous mapping named
`maton-gpu-bootstrap`, with the version magic, bridge/adoption pointers, and the
kernel AT_HWCAP2 FSGSBASE permission. `bootstrap.h` defines this small ABI.
The ICD discovers it via `/proc/self/maps`, without `environ`, getenv, auxv
libc helpers, fixed virtual addresses, or reserved descriptors. The launcher
continues publishing environment variables for the older spike clients.
Missing publication/adoption fails with a diagnostic, not a wrong-TLS call.
Read access to the process's maps is required; unavailable proc access fails
closed. This discovery is process-local plumbing, not a security boundary.

Only `arch/x86_64/backend.h` contains CPU instructions/syscall numbers. The
backend switches only the FS pointer while resident Android Vulkan calls run.
Compiler atomic builtins provide synchronization. Arm64/riscv64 backends are
compile-only, fail-closed slots; the core, validators and generated thunks compile
for both targets, but runtime TPIDR_EL0/tp switching/adoption is not implemented.

The alleged stage-A FSGSBASE denial could not be reproduced. A rebuilt untouched
stage-A benchmark, run under the untouched stage-A launcher, already exercises
WRFSBASE (157.4 ns versus 664.5 ns arch_prctl). The guest's `/proc/self/auxv` has
AT_HWCAP2=2; the new probe also reads it correctly. No kernel/auxv permission fix
was justified. The new ICD selects WRFSBASE from the launcher's kernel permission
publication. Latest 100,000-pair measurement: **163.9 ns WRFSBASE**, **639.7 ns
arch_prctl**, including adoption lookup, excluding Vulkan.

## Build and reproduce

Keep all build products outside the repository. Prerequisites: clang/llvm-readelf
21, Python, glslangValidator (test shaders), CMake and host C/C++ tools.
`pins.json` pins Headers/Loader/Tools v1.4.341 tarballs and vk.xml. Stage-B tests
reuse the unchanged pinned source-built glibc loader/tools from stage A.

```sh
OUT=/absolute/external/output
CMAKE=/path/to/cmake bash linux/vulkan-forward/build-tools.sh "$OUT"
HEADERS="$OUT/src/Vulkan-Headers-1.4.341"
bash linux/vulkan-forward/tests/run.sh "$OUT/build" "$HEADERS"
bash linux/vulkan-forward/tests/build-offscreen.sh "$OUT/tests" "$HEADERS" "$OUT/loader-build"
```

`build.sh` checks ELF dependencies and cross-compiles runtime/core/generated code
for aarch64-linux-gnu and riscv64-linux-gnu. `tests/run.sh` checks the three exports,
zero undefined symbols, helper/allocator behavior (8 threads × 1,000 iterations),
adoption ABI offsets, and absent-bridge refusal. Test shaders are compiled to
SPIR-V and embedded at build time; this run used glslang 15.1.0.

Build `gpu-forward-spike/spike3/build-android.sh` from an external scratch copy of
`gpu-forward-spike`, `compat-proxy`, and `packs`, preserving sibling layout.
It builds the NDK r30 `-DMATON_ADOPT` launcher. Push that launcher as `launcher-b`,
our ICD/tests and the pinned `vulkaninfo` under `/data/local/tmp/vk/`. Supply glibc's
`ld-linux-x86-64.so.2`, libc/libm/libstdc++/libgcc_s and the pinned
`libvulkan.so.1.4.341` (`libvulkan.so.1` symlink) in that directory's `lib/`.
The ICD JSON (`maton.json`) is:

```json
{"file_format_version":"1.0.0","ICD":{"library_path":"/data/local/tmp/vk/libmaton_vulkan.so","api_version":"1.4.0"}}
```

Run inside `/data/local/tmp/vk/`:

```sh
VK_ICD_FILENAMES=/data/local/tmp/vk/maton.json ./launcher-b ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./vulkaninfo --summary
VK_ICD_FILENAMES=/data/local/tmp/vk/maton.json ./launcher-b ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./offscreen
./launcher-b ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./query-test ./libmaton_vulkan.so
./launcher-b ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./switch-bench
MATON_QUERY_NO_ADOPT=1 ./launcher-b ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./query-test ./libmaton_vulkan.so
```

The missing-adoption test asks the launcher to omit adoption from its publication;
unsetting the app's environment after startup no longer changes that capability.
No APK installation, protected app data, reboot, or image change is needed.
If deleting large VM artifacts, run `sm fstrim` afterwards.

## Results and remaining scope

All acceptance commands pass. Device: `llvmpipe (LLVM 21.1.8, 256 bits)`, API
1.4.354. Four adopted threads each fill/dispatch/read back 1,024 correct uints.
Queue submissions are mutex-serialized as Vulkan requires for a shared queue;
fence waits and resource work run concurrently. Two threads use submit2; one
executes a secondary buffer. Wrapper callbacks verify an app TLS sentinel and
balance at 11/11 (including a rejected callback-chain device creation).
Queue getters return the same wrapper and device-group creation succeeds.
Triangle: 64×64 RGBA8, 1,152 red pixels, byte sum 1,338,240,
FNV-1a `66d859cf714c4d25`; every pixel's channels are checked.
No validation layers were built or run.

The explicit extension list follows [Mesa's Zink requirements](https://docs.mesa3d.org/drivers/zink.html)
plus optional dynamic-state/pipeline paths. It deliberately excludes
`VK_KHR_swapchain_mutable_format` and the external-memory/DRI path. Stage C
verifies offscreen Zink using Mesa profiles and surfaceless EGL, and verifies
libc independence with a dynamic musl loader; details are in STAGE-C.md. Desktop
presentation still requires the later WSI/ANativeWindow/AHB work.

Production packaging is wired through the host APK build. `linux/gl-maton/`
builds the forwarder into `org.freedesktop.Platform.GL.maton`, mounted at
`/usr/lib/x86_64-linux-gnu/GL/maton`. The launcher selects it through
`FLATPAK_GL_DRIVERS=maton:default` and passes `--gpu-bridge` to the
Android-linked production loader. The ICD manifest follows the runtime's
`lib/vulkan/icd.d/` extension layout.
The forwarder then calls the Android Vulkan loader and its selected device
(lavapipe on the acceptance VM). Apps still receive the existing `--device=dri`
grant. This path is offscreen only: desktop presentation remains the separate
WSI stage.

Still unverified: real GPU drivers, signal/reentrant calls during a
switched-TLS call, extension execution beyond the exercised paths, allocation
failure injection, and runtime architectures other than x86_64. The Flatpak
acceptance check must use the actual stub launch path on a freshly booted image.
No shared-tree or kernel changes are required.

## Wayland WSI prototype

See [WSI.md](WSI.md) for the agreed dmabuf and explicit-sync design and current
implementation status. `tests/run-wsi.sh OUT HEADERS` runs the internal host
model and freestanding Wayland-symbol resolver checks. The prototype build does
not expose WSI extensions or Vulkan surface/swapchain commands. Default
offscreen behavior remains the stages A–C interface.

## Production stub verification, 2026-10-19

On enforcing v39 / adb 5556, with the linker-exempt proxy plus the resident
altstack fix, the actual VulkanInfo stub reaches the freestanding forwarder.
Diagnostics confirm bridge `dlopen("/system/lib64/libvulkan.so")` succeeds,
resident `vkCreateInstance` returns VK_SUCCESS, and resident
`vkEnumeratePhysicalDevices` returns VK_SUCCESS with count zero. VulkanInfo
therefore exits 1 with ERROR_INITIALIZATION_FAILED; no device-listing success
is claimed.

The independent bionic `tests/native-loader-probe.c` reproduces exactly the
same successful creation and zero-device enumeration without either the proxy
or forwarder. Compile it using the NDK x86_64 API-36 compiler, pinned Vulkan
headers, and `-ldl`, push the resulting test executable to `/data/local/tmp/`,
and execute it directly through root adb. It does not change driver settings.
The VM reports `ro.hardware.vulkan=virtio` and provides renderD128. The current
blocker is the native VM Vulkan path, not forwarder enumeration. A coordinator
must provide a VM/image with a native working Vulkan device (or decide on the
appropriate software fallback); this task did not reboot or alter the image.
Retest the native probe first, then force-stop and launch the existing stub
with cache/v3-vkdebug enabled; verify deviceName/vendorID/deviceID/apiVersion in
its MatonV3 output. Real hardware likewise needs a working native Vulkan HAL;
this run establishes no real-hardware result.

Validation: signed standalone build, full compat-proxy suite, packs suite and
`MATON_AOSP_ROOT=/mnt/data/aosp tests/run.sh OUT` pass. The additional forwarder
suite passes build/runtime/ABI/absent-bridge checks but its switch-bench test
fails to compile because it still references the removed adopt_thread.selector
field; that separate test mismatch is not changed here. The forwarder build
retains three exported ICD symbols, no libc and no DT_NEEDED dependencies.
Evidence is outside the repo in sigsys3-out/segv-evidence. No shared-tree or
kernel changes are required by the proxy fix; VM driver configuration remains
a coordinator decision.
