# Maton Flatpak GL extension

The subsystem APK carries the x86_64 forwarders as the Flatpak extension
`org.freedesktop.Platform.GL.maton`. The selected `26.08` branch matches the
assigned runtime. Its runtime metadata advertises
`[Extension org.freedesktop.Platform.GL] versions=26.08;26.08-extra;1.4`,
`directory=lib/x86_64-linux-gnu/GL`, `subdirectories=true`, and
`add-ld-path=lib`. The `26.08` branch is checked against that metadata at
launch; a mismatch fails with a clear error rather than silently selecting an
incompatible extension.

GLVND loads `libGLX_maton.so.0` by the `__GLX_VENDOR_LIBRARY_NAME=maton`
selection when the Maton GL extension is enabled. It renders through the EGL
forwarder into a pbuffer and copies frames to the X window with XPutImage. The
extension payload uses the runtime's GL `merge-dirs` layout (the
Platform metadata lists `vulkan/icd.d;glvnd/egl_vendor.d;...` relative to the
extension root; flatpak merges only those into `GL/vulkan/icd.d` etc.):

```text
lib/libEGL_maton.so
lib/libGLX_maton.so.0
# libGLX_maton.so is a regular duplicate for OSTree compatibility
lib/libGLX_maton.so
lib/libmaton_vulkan.so
vulkan/icd.d/maton_icd.json
glvnd/egl_vendor.d/10_maton.json
```

The ICD paths resolve to the extension directory and its loader path. Both
forwarders remain freestanding and have no `DT_NEEDED` entries. Build task
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
