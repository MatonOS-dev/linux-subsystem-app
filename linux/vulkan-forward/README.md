# Vulkan forwarder — stage A (query-only)

Linux ICD experiment forwarding instance/physical-device queries to the resident
Android Vulkan loader in a MatonOS system based on AOSP. **Not ready for Flatpak
use:** the unmodified Khronos loader rejects this stage because it has no device
entry points. The requested `vulkaninfo --summary` result is blocked (see below).
No compute, render, musl, WSI or Zink work is part of this stage.

## Design

`icd.c` exports only the three ICD entry points and negotiates interface 5.
`gen.py` verifies the pinned `vk.xml` SHA-256 before emitting 18 physical query
thunks, dispatch-table fields and a 28-command lookup table. Selection is core
Vulkan 1.0–1.4, instance and physical-query level; extension allowlist is empty.
All WSI/surface/swapchain/device commands return NULL. Proc lookup also asks the
Android loader, so an absent driver command is never returned. The Android
instance version is capped at 1.4; property/feature/query payloads come directly
from the driver. Empty extension lists are the intersection with our empty
allowlist; no extension or layer support is invented. This is an incomplete
query-only API, **not** a conformant Vulkan 1.4 implementation.

Instances and physical devices have allocated wrappers headed by
`VK_LOADER_DATA`, initialized with the ICD magic expected by the loader.
Physical-device wrappers are cached per instance under an atomic lock; repeated
and group enumeration return identical wrappers. Group arrays are rewritten on
the Linux side, including their returned dispatchable handles. Android handles
never escape these paths. Other generated physical queries have no nested
input/output dispatchable handles; generation rejects unhandled direct handle
parameters. This generator is deliberately not a general device/pNext adapter.

`MATON_GPU_BRIDGE` provides Android dlopen/dlsym. `MATON_THREAD_ADOPT` is the
spike-3 v1 adoption API. Initialization opens `/system/lib64/libvulkan.so` on
resident TLS and caches instance query pointers during instance creation.
Generated thunks are plain C: `switch_to_android()`, typed call, `switch_back()`.
Missing bridge/adoption fails with a diagnostic and NULL/error; there is no
owner-thread-only fallback. Per-thread syscall-dispatch selector state is saved
and restored with TLS. No Linux allocation callback reaches Android: Android
creation/destruction use NULL callbacks. App callbacks allocate/free our wrappers
on app TLS, or the mmap allocator supplies them. Instance create pNext is removed
because this stage enables no extensions and must not forward Linux callbacks.

The x86_64 backend owns all CPU instructions and raw syscall numbers. It uses
WRFSBASE only when the kernel's AT_HWCAP2 grants it, otherwise arch_prctl. Its
minimal adoption-ABI prefix is checked against the provider's real header by
`abi-test.c`; it never inspects libc TCBs. Arm64/riscv64 headers are compile-only,
fail-closed stubs: later implementations need TPIDR_EL0/tp and real adoption.
No arch ifdefs or assembly appear in core/generated code. Atomics use compiler
builtins. The syscall mmap-per-allocation allocator has checked size addition,
max_align_t alignment, no shared mutable state or arbitrary arena/thread cap.

The DSO builds with `-ffreestanding -nostdlib -fno-stack-protector -fno-builtin
-fvisibility=hidden`. No libc code is linked or bundled, no libc functions or
compiler TLS are imported, and it has **zero DT_NEEDED**. One weak data symbol,
`environ`, reads the standard process environment supplied by the app's libc;
glibc and musl both provide this data ABI. If it is unavailable, initialization
fails closed. This is a deliberate data-only bootstrap dependency, not a claim
of zero undefined symbols. Static musl loading remains untested, deferred with
the rest of stage B/C.

## Reproduce

All source downloads/build products go outside this repository. Prerequisites:
clang 21, llvm-readelf 21, Python 3.12+, C/C++ host compilers, CMake >=3.22.1,
curl, ripgrep. Dependency tarballs and vk.xml are pinned in `pins.json`.

```
OUT=/path/out/vk-forward
CMAKE=/path/to/cmake bash linux/vulkan-forward/build-tools.sh "$OUT"
bash linux/vulkan-forward/tests/run.sh "$OUT/build" "$OUT/src/Vulkan-Headers-1.4.341"
```

`build.sh` builds the DSO, checks DT_NEEDED, and compiles both core units and
all generated code with `--target=aarch64-linux-gnu` and `riscv64-linux-gnu`.
`tests/run.sh` checks the exports/undefined symbol, exercises helpers and
allocation from eight threads, validates the adoption ABI, and checks missing
bridge refusal. `coverage.txt`, `dynamic.txt`, and `symbols.txt` are output evidence.
No AOSP, Gradle, kernel, image, APK or subsystem/Java changes are needed.

Build the spike launcher with `spike3/build-android.sh` in a scratch copy of
`linux/gpu-forward-spike`, `linux/compat-proxy`, and `linux/packs` (preserve sibling
layout). This existing script writes its own build directories, so use a copy
outside the repository. It compiles with NDK r30 and `-DMATON_ADOPT`.

Use only `/data/local/tmp/vk/` in the already-running VM. Push the launcher as
`launcher`, `libmaton_vulkan.so`, `query-test`, `switch-bench`, and `vulkaninfo`.
Push host `ld-linux-x86-64.so.2` there, and host libc/libm/libstdc++/libgcc_s plus
the pinned loader's `libvulkan.so.1.4.341` into `lib/`. Add
`lib/libvulkan.so.1 -> libvulkan.so.1.4.341`. JSON:

```
{"file_format_version":"1.0.0","ICD":{"library_path":"/data/local/tmp/vk/libmaton_vulkan.so","api_version":"1.4.0"}}
```

Inside that directory, launch each glibc test via:

```
./launcher ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./query-test ./libmaton_vulkan.so
./launcher ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./switch-bench
MATON_QUERY_NO_ADOPT=1 ./launcher ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./query-test ./libmaton_vulkan.so
VK_ICD_FILENAMES=/data/local/tmp/vk/maton.json VK_LOADER_DEBUG=error,warn ./launcher ./ld-linux-x86-64.so.2 --inhibit-cache --library-path /data/local/tmp/vk/lib ./vulkaninfo --summary
```

## Results and blocker (2026-10-17)

Host helper, allocator concurrency, ABI, fail-closed, ELF and cross-compile checks
PASS. Direct ICD VM tests PASS: one `llvmpipe (LLVM 21.1.8, 256 bits)` device,
driver API 1.4.354, instance API capped to 1.4.0, properties via pNext,
features, format/memory/queue queries, stable physical/group handles, default
allocator plus app allocator (2 allocations/2 frees).

100,000 TLS round trips on this VM: arch_prctl **654.0 ns/call**, WRFSBASE
**157.7 ns/call**. Includes adoption lookup and selector exchange/restore;
excludes actual Vulkan execution. VM noise makes this an observation, not a
performance guarantee. No validation layers were built.

**`vulkaninfo --summary` FAILS:** pinned Khronos Vulkan-Loader 1.4.341 logs
`Unable to load vkGetDeviceProcAddr` then `Failed to find required entrypoints`
and `vkCreateInstance failed with ERROR_INCOMPATIBLE_DRIVER`. Independently,
Vulkan-Tools 1.4.341 `AppGpu` creates a logical device and image resources before
printing even the summary. Meeting that acceptance test requires device-level
forwarding/wrapping and the resource-query path, beyond this stage's
instance/physical-only scope. The loader/tools were not patched to mask this.
No full-loader success, compute/render/musl coverage, or deployability is claimed.

## Future packaging (documentation only)

The existing `gpu-forward-spike/flatpak/metadata.in` describes
`org.freedesktop.Platform.GL.maton`. In that GL extension root install
`lib/libmaton_vulkan.so` and `share/vulkan/icd.d/maton_icd.json`, substituting
`@GL_EXTENSION_DIR@` in the JSON template with its mounted extension directory.
That template now records the 1.4 registry ceiling. The Linux app must be started
through the adoption-capable launcher and given that JSON only when permitted
`--device=dri`/all. Other apps keep Mesa software. No packaging or permission
plumbing has been enabled in this stage; do not ship this query-only ICD as a
working replacement. Future device forwarding must wrap device/queue/command
buffer handles and handle dispatchable handles inside relevant pNext chains.

Open risks: full Linux-loader device initialization, arm64/riscv64 adoption,
static-musl loader/bootstrap, signal/reentrancy behavior during driver calls,
real app SELinux/seccomp confinement, and non-lavapipe hardware are unverified.
No shared-tree or kernel changes requested. Zink investigation is deferred.
