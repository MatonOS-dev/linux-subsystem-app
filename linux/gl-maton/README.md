# Maton Flatpak GL extensions

The subsystem APK carries the x86_64 forwarders as the Flatpak extension
`org.freedesktop.Platform.GL.maton`. The selected `26.08` branch matches the
assigned runtime. Its runtime metadata advertises
`[Extension org.freedesktop.Platform.GL] versions=26.08;26.08-extra;1.4`,
`directory=lib/x86_64-linux-gnu/GL`, `subdirectories=true`, and
`add-ld-path=lib`. The `26.08` branch is checked against that metadata at
launch; a mismatch fails with a clear error rather than silently selecting an
incompatible extension.

The APK also carries the i386 forwarders as a separate
`org.freedesktop.Platform.GL32.maton` extension. Its extension point mounts at
`/usr/lib/i386-linux-gnu/GL/maton`, so its payload uses the same relative
`lib/` layout as the x86_64 extension; it must not add another
`i386-linux-gnu/` level below that mount.

GLVND loads `libGLX_maton.so.0` by the `__GLX_VENDOR_LIBRARY_NAME=maton`
selection when the Maton GL extension is enabled. It renders through the EGL
forwarder into a pbuffer and copies frames to the X window with XPutImage. The
extension payload uses the runtime's GL `merge-dirs` layout (the
Platform metadata lists `vulkan/icd.d;glvnd/egl_vendor.d;...` relative to the
extension root; flatpak merges only those into `GL/vulkan/icd.d` etc.):

```text
# x86_64 forwarders
lib/libEGL_maton.so
lib/libGLX_maton.so.0
# libGLX_maton.so is a regular duplicate for OSTree compatibility
lib/libGLX_maton.so
lib/libmaton_vulkan.so
vulkan/icd.d/maton_icd.json
glvnd/egl_vendor.d/10_maton.json

# i386 ICD in the standard GL extension
lib/libmaton_vulkan_i386.so
vulkan/icd.d/maton_icd_i386.json
glvnd/egl_vendor.d/10_maton_i386.json

# nested i386 EGL/GLX for multiarch x86_64 runtimes
lib/i386-linux-gnu/libEGL_maton.so
lib/i386-linux-gnu/libGLX_maton.so.0
lib/i386-linux-gnu/libGLX_maton.so
```

The separate GL32 extension contains:

```text
lib/libEGL_maton.so
lib/libGLX_maton.so.0
lib/libGLX_maton.so
lib/libmaton_vulkan_i386.so
vulkan/icd.d/maton_icd_i386.json
glvnd/egl_vendor.d/10_maton_i386.json
```

The GL32 EGL manifest selects `libEGL_maton.so` from the extension's `lib/`
directory, which is mounted at `/usr/lib/i386-linux-gnu/GL/maton/lib`. The
standard GL extension's i386 manifest selects its nested i386 DSO. Both
architectures' forwarders remain freestanding and have no `DT_NEEDED` entries.
Build task
`buildGlMaton` commits the extension files to a local OSTree repo, then uses
the existing `linux/packs/pack.py` format. `packageGlMaton` places the ref
metadata, index, OSTree objects and immutable content packs in the signed
subsystem APK under `assets/gl-maton/`. The launcher mounts that package
through the existing loader pack interface at
`/usr/lib/x86_64-linux-gnu/GL/maton`; Flatpak receives
`FLATPAK_GL_DRIVERS=maton:default`. Vulkan and EGL environment overrides and
the old `/run/maton` maps are removed. The existing `--gpu-bridge` path is
unchanged.

At launch, the host creates the matching
`runtime/org.freedesktop.Platform.GL.maton/x86_64/26.08/<commit>` deployment
under Flatpak's temporary system installation, using the packaged ref,
metadata, and OSTree commit object. It includes the normal `files/`,
`metadata`, valid Flatpak deployment record, `.ref`, `active`, and `current`
entries. The signed payload pack overlays that deployment's `files/` directory
through the proxy; there is no separate `/usr/lib/.../GL/maton` outer-view
mount. `FLATPAK_GL_DRIVERS=maton:default` is set on Flatpak itself for
extension selection and in the app environment for consistent driver choice.
Flatpak can therefore discover the driver as an installed GL extension and
generate its normal GL driver paths.

The package is built from the same forwarder outputs as the subsystem APK,
so subsystem APK updates replace its payload. It is not published to Flathub.
To select a different runtime branch, set `MATON_GL_RUNTIME_BRANCH` during the
subsystem build. The runtime metadata check still gates launch on the selected
branch being declared by the runtime's GL extension point.

## Tests and device check

`tests/run.sh` builds a test OSTree ref, verifies the ref branch, metadata,
payload paths and pack contents. The subsystem host test verifies the
temporary installed-ref layout and commit object. The top-level
`tests/run.sh` runs this test alongside the pack and compatibility-proxy
suites. `V3Launcher` checks the runtime's GL extension version list before
adding the payload pack mount.

Host acceptance checks ELF32 class, the expected EGL/GLX exports, absence of
`DT_NEEDED`, EGL proc aliases, and the packaged i386 vendor manifest and
multiarch paths. The full APK build's ISA audit must retain the i386 `corei7`
baseline.

### i386 Platform compatibility-extension test

On a fresh image with the i386 Platform compatibility extension available,
install the signed subsystem APK, then launch 32-bit `eglinfo`, `glxinfo`, and
`vkcube` from generated Flatpak stubs. Confirm each executable is ELF32 and
uses the i386 GL paths under `/usr/lib/i386-linux-gnu/GL`; `eglinfo` should
list Maton and the resident GLES renderer, `glxinfo` should report GLX 1.4 and
create a context, and `vkcube` should enumerate the i386 Vulkan ICD and create
its rendering surface. Capture `logcat -s MatonV3` and each command's output.
If the runtime's i386 compatibility extension does not include Vulkan, record
that environment limitation and still run the EGL and GLX checks.

Assigned-VM acceptance procedure (ADB `127.0.0.1:5556`; do not reboot):

1. Install the signed `MatonLinuxSubsystem.apk` produced by `build.sh`.
2. Force-stop `flatpak.app.org_freedesktop_Platform_VulkanInfo.x86_64.v26_08.x39bef4a9006b`.
3. Relaunch it with `monkey -p flatpak.app.org_freedesktop_Platform_VulkanInfo.x86_64.v26_08.x39bef4a9006b 1`.
4. Check `logcat -s MatonV3` for VulkanInfo discovering the Maton ICD and
   listing the forwarder device. Check that EGL's vendor list includes Maton.
   If Vulkan reaches the driver but fails later, record that next error.

VM result (v39, 2026-10-19): Flatpak's bwrap trace mounts the deployed ref at
`/usr/lib/x86_64-linux-gnu/GL/maton`, and its generated
`runtime-002-org.freedesktop.Platform.GL.maton.conf` is present. The Vulkan
loader reports:

```text
Found ICD manifest file /usr/lib/x86_64-linux-gnu/GL/vulkan/icd.d/maton_icd.json, version 1.0.0
Searching for ICD drivers named libmaton_vulkan.so
loader_scanned_icd_add: Could not get 'vkCreateInstance' via 'vk_icdGetInstanceProcAddr' for ICD libmaton_vulkan.so
```

`vulkaninfo` then exits with `vkCreateInstance failed with
ERROR_INCOMPATIBLE_DRIVER`. This confirms extension discovery and library
loading; forwarder initialization is the next separate issue.

Assigned VM result (v45, 2026-10-24): the `25.08` i386 compat runtime and
`org.freedesktop.Platform.GL32.maton` were both mounted. The GL32 tree was
mounted at `/usr/lib/i386-linux-gnu/GL/maton` with its `lib/` directory and
merged ICD/vendor metadata at the expected locations. A test-only Flatpak
`features=multiarch` override allowed ELF32 payloads through `setup_seccomp`;
the hook used to write this override was removed after testing and is not in
the product APK. The EGL and GLX probes reached the Maton EGL forwarder, which
reported `GPU bootstrap bridge or adoption API missing`; EGL returned
`EGL_BAD_DISPLAY`, and no GLX pixel-readback pass was obtained. The Vulkan
probe was launched as ELF32 but did not enumerate a usable Maton device.

The remaining blocker is cross-ABI GPU adoption. The bootstrap currently
publishes x86_64 bridge/adoption function addresses, while ELF32 forwarders
parse them into 32-bit pointers and cannot call x86_64 code in 32-bit mode.
The i386 adoption backend is not wired to an i386-callable resident bridge.
Completing this needs a 32-bit callable bridge or an explicit cross-ABI RPC
gateway, plus working bionic TLS registration for the payload threads; the
GL32 packaging fix alone cannot provide rendering.
