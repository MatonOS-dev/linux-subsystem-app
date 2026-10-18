# Stage C — musl and Zink offscreen

Stage C passes on the existing localhost:5556 lavapipe VM. No WSI, subsystem,
Java, image, kernel or packaging activation changes are included. MatonOS is
based on AOSP. Worktree: `lsa-vk`, branch `vk-forward`, base `384566a`.

## Architecture and coverage

The stage-B architecture is unchanged: dispatchable handles wrap Android
handles behind a loader dispatch slot; typed generated C thunks unwrap inputs,
enter resident bionic TLS, call cached Android entry points and restore Linux
TLS. The adoption launcher supplies resident threads and the process-local
bootstrap mapping. Both glibc and musl load exactly the same ICD DSO, not
separate builds. No libc is linked/bundled in the ICD. Its ELF still has zero
DT_NEEDED, zero undefined symbols, exactly three exports. Runtime and generated
code compile freestanding for arm64/riscv64 (runtime backends remain stubs).
Android receives NULL allocation callbacks; app callbacks own wrappers on app TLS.

Coverage remains 283 canonical commands (234 core), 68 aliases, 262 generated
thunks, 21 manual adapters. The allowlist grows from 69 to **71** device
extensions: `VK_KHR_shader_float_controls2` and
`VK_EXT_legacy_vertex_attributes`. Both were first observed in the resident
driver inventory; both contain only feature/property structures, no new commands,
callbacks or handles. Enumeration still intersects the actual driver's list.
Instance ICD extensions remain empty. No surface/swapchain extension is exposed.

100,000 TLS switch pairs on this run: **158.7 ns WRFSBASE**, **645.5 ns
arch_prctl**, including adoption lookup, excluding Vulkan work.
VM timings are noisy and are not an end-to-end performance claim.

## Pins and environment

All artifacts/logs are in `/home/hanro50/matonos/build/vk-forward-c-out`.
Builds use at most four jobs, no files in the repository as output.

| Dependency | Version / SHA-256 of source archive |
| --- | --- |
| Mesa | 26.2.3 / `1628058a8d2c0615975de5a15ab7bbb9638c50000b5bed9456ff423ea034a81f` |
| musl | 1.2.6 / `e9db8b70b0cba3db43b27e84ccd8a0c0958b40029a1c36c9f13817b93787b4d6` |
| libdrm | 2.4.134 / `ac5e74d157830eb8bee44c6a6bf3ad49774ef0dd2a72bdad74a8f20308b52a95` |
| Vulkan-Profiles | Mesa-pinned `a1e185fb5035f9726d16a869a2f11067dd21aa10` ZIP / `775b0faebd53e3f31f1771d7ca9f8d899778ce596177fb9edd8d26d5323f854a` |

Khronos Headers/Loader/Tools stay at v1.4.341 with archive and registry hashes in
`pins.json` (unchanged from A/B). Host glibc is 2.39; GCC 13.3, clang/llvm-readelf
21.1.8, CMake 4.1.2, Meson 1.9.1, glslang 15.1.0. Launcher/probe use NDK r30 API
36. VM reports llvmpipe LLVM 21.1.8, Vulkan API 1.4.354. The Profiles revision is
an intentional exception to newest-stable preference: use the exact revision
pinned by the selected Mesa release to audit its own profiles.

Musl is built from source; its libc DSO is also its dynamic interpreter.
The pinned Khronos loader is rebuilt **dynamically against musl**, with WSI off.
`offscreen-musl` needs only `libvulkan.so.1` and `libc.so`; that loader needs only
`libc.so`. No glibc is loaded into this musl process. The ICD initially tested
here was byte-identical to stage B (SHA-256
`dfebe59dd4de8b147bf125e9a19a86095a6f5c99b797a9a40fa965de62006421`);
all final tests rerun with the same new 71-extension DSO for both libcs.
Musl vulkaninfo was not built: Vulkan-Tools requires a musl C++ runtime/toolchain;
the requested compute acceptance, including its extra triangle test, is covered.

Mesa builds only Zink, surfaceless EGL, no Vulkan drivers, GBM, GLX or window
platforms, LLVM disabled. libdrm is a userspace Mesa build dependency; this test
uses neither a Linux render node nor FD/external-memory interop. Host expat/zlib/
zstd and the stage-B glibc runtime are supplied alongside Mesa on the guest.

## Reproduction

Prerequisites are documented in README.md. `DEPS` is the verified stage-A output
containing source-built Headers/Loader/Tools. Run from this worktree:

```sh
OUT=/home/hanro50/matonos/build/vk-forward-c-out
DEPS=/mnt/data/aosp/out/pc-logs/vk-forward
export CMAKE=/home/hanro50/Android/Sdk/cmake/4.1.2/bin/cmake
bash linux/vulkan-forward/tests/build-musl.sh "$OUT" "$DEPS"
bash linux/vulkan-forward/tests/build-zink.sh "$OUT"
bash linux/vulkan-forward/tests/run.sh "$OUT/build" "$DEPS/src/Vulkan-Headers-1.4.341"
bash linux/vulkan-forward/tests/build-profile-audit.sh "$OUT" "$DEPS"
```

Build the adoption launcher through `spike3/build-android.sh` from an external
scratch copy of `linux/{gpu-forward-spike,compat-proxy,packs}`, preserving their
sibling layout under `$OUT/launcher-src/linux/`. This run rebuilt it that way.
Stage-B glibc runtime, vulkaninfo and JSON must already be installed under
`/data/local/tmp/vk/` as described in README.md. Then:

```sh
ADB=/mnt/data/aosp/out/host/linux-x86/bin/adb \
 bash linux/vulkan-forward/tests/run-stage-c-vm.sh "$OUT"
ADB=/mnt/data/aosp/out/host/linux-x86/bin/adb \
 bash linux/vulkan-forward/tests/run-profile-vm.sh "$OUT"
```

The script pushes only to `/data/local/tmp/vk/`; retains guest files; it does not
install APKs, access app data, reboot, or alter the VM lifecycle. Shader caching
is disabled, so Mesa does not write to its default HOME outside that directory.
For real devices: use a spare test device with the adoption launcher and the
same isolated runtime/JSON, repeat this suite, verify the renderer names that
device, and retain capability/profiles output. No real-hardware pass is claimed.

`MESA_LOADER_DRIVER_OVERRIDE=zink` is essential. `GALLIUM_DRIVER=zink` alone
allowed Mesa's EGL software fallback to initialize the wrong configuration cache
and assert in driQueryOptionb before instance creation. The override resolves
this through configuration, without a Mesa patch. `LIBGL_ALWAYS_SOFTWARE=1`
selects the VM's CPU Vulkan device; remove that selection for future real-GPU
experiments. No GL version override or forced Vulkan feature bit is used.

## Results

`vm-suite.txt` and individual `*-vm.txt` files retain complete output. The suite
exited zero. `vulkaninfo --summary` still lists llvmpipe API 1.4.354.

```text
Enabled 71 allowlisted driver extensions
PASS compute adopted thread 0: fill + dispatch + 1024 readbacks
PASS compute adopted thread 1: fill + dispatch + 1024 readbacks
PASS compute adopted thread 2: fill + dispatch + 1024 readbacks
PASS compute adopted thread 3: fill + dispatch + 1024 readbacks
PASS triangle: red pixels=1152 sum=1338240 FNV1a=66d859cf714c4d25
PASS app-TLS wrapper allocator callbacks 11/11; device-group pNext, queue identity, submit/submit2, secondary buffers
PASS offscreen compute/render (musl 1.2.6, pinned Khronos loader)
```

The glibc test passes the same checks. Thread ordering varies; queue submissions
are externally synchronized as Vulkan requires. Original stage-B shaders are
compiled to embedded SPIR-V at build time. Final test output identifies libc
through a build-time label rather than the old hard-coded glibc text.

| Zink context | Exposed GL | GLSL | FBO result |
| --- | --- | --- | --- |
| desktop core | 4.6 Core Profile Mesa 26.2.3 | 4.60 | PASS |
| desktop compatibility | 4.6 Compatibility Profile Mesa 26.2.3 | 4.60 | PASS |
| GLES | OpenGL ES 3.2 Mesa 26.2.3 | OpenGL ES GLSL ES 3.20 | PASS |

All expose EGL 1.5, vendor Mesa Project, renderer
`zink Vulkan 1.4(llvmpipe (LLVM 21.1.8, 256 bits) (MESA_LLVMPIPE))`.
Each uses an EGL_NO_CONFIG_KHR context with no surface, an explicit 64x64 RGBA8
FBO, GLSL-compiled triangle, glReadPixels, full channel validation, 1,152 red
pixels, sum 1,338,240 and FNV-1a `66d859cf714c4d25`. The shader source differs for
desktop GLSL 330 and GLES 300. No pbuffer, native window or WSI is involved.

Host runtime/allocator/adoption refusal checks, ELF export/dependency checks and
arm64/riscv64 freestanding compile checks pass. No validation layers were built
or run. These are targeted smoke/readback tests, not CTS/conformance coverage.

## Requirements and limitations

`build-profile-audit.sh` builds Mesa's unmodified Zink requirements checker using
Mesa's exact pinned Vulkan-Profiles generator and Mesa's own newer registry for
that auxiliary checker (not the ICD). It also builds it natively for the
resident driver, without the Linux ICD. Native debug messages are redirected to
stderr through the upstream callback hook. Generator logs include an upstream
profile-validation diagnostic about maintenance5 properties in gl46_optimal;
it still emits the header and the baseline checks run. This limits how strongly
the optimal-profile result should be interpreted.

Unmodified resident-driver profiles pass every GL 2.1–4.6 baseline. The Linux
ICD's unmodified profiles fail because they demand
`VK_KHR_incremental_present` and `VK_KHR_swapchain_mutable_format`, intentionally
hidden WSI. This does not stop surfaceless Zink. A separately labeled diagnostic
copy removes only these two requirements to evaluate offscreen features/formats/
limits; all 12 GL 2.1–4.6 baseline profiles then pass, with no failed baseline feature,
format or limit check. It is not an upstream profile/conformance pass. See
`profiles-forwarded.txt`, `profiles-android.txt`, and
`profiles-offscreen-forwarded.txt` for full diagnostics.

The optimal profile additionally requires four extensions **absent in the
resident driver inventory**: `VK_KHR_maintenance7`, `VK_KHR_maintenance8`,
`VK_KHR_maintenance10`, `VK_KHR_device_address_commands`. They are never claimed.
The last also lies beyond the ICD's pinned registry. These are unavailable
optional optimization paths; they do not reduce the exposed GL/GLES version in
this test. The two ICD-hidden optimization features fixed in this stage are
float_controls2 and legacy_vertex_attributes; no synthetic support was added.

`audit-zink.py` inventories every extension referenced by the pinned Zink device
code: 114 names, 56 forwarded, 40 driver-reported but hidden, 18 driver-absent.
`zink-extension-audit.md` records each name and cause, including deliberately
excluded WSI/external-handle paths and optional unimplemented paths. It measures
extension-name availability, not feature-bit support or executed coverage for
all optional paths. Only the documented profile checks and GL tests establish
feature/format/limit behavior; absence of a diagnostic is not proof for every
optional OpenGL extension. Keep this allowlist reviewed rather than blanket
exposing external handles, callbacks or unwrapped dispatchable structures.

Remaining work: WSI/ANativeWindow/AHB, desktop presentation, external-memory/FD
paths, actual Flatpak/seccomp/SELinux context, real GPUs, signal/reentrancy safety,
allocation fault injection and real non-x86 backends. No AOSP patch, shared-tree
change, kernel change, push or merge is needed for this stage.
