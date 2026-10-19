# Maton Flatpak GL extension

The subsystem APK carries the x86_64 forwarders as the Flatpak extension
`org.freedesktop.Platform.GL.maton`. The selected `26.08` branch matches the
assigned runtime. Its runtime metadata advertises
`[Extension org.freedesktop.Platform.GL] versions=26.08;26.08-extra;1.4`,
`directory=lib/x86_64-linux-gnu/GL`, `subdirectories=true`, and
`add-ld-path=lib`. The `26.08` branch is checked against that metadata at
launch; a mismatch fails with a clear error rather than silently selecting an
incompatible extension.

The extension payload mirrors the deployed `GL.default` layout:

```text
lib/libEGL_maton.so
lib/libmaton_vulkan.so
lib/vulkan/icd.d/maton_icd.json
share/glvnd/egl_vendor.d/10_maton.json
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

The package is built from the same forwarder outputs as the subsystem APK,
so subsystem APK updates replace its payload. It is not published to Flathub.
To select a different runtime branch, set `MATON_GL_RUNTIME_BRANCH` during the
subsystem build. The runtime metadata check still gates launch on the selected
branch being declared by the runtime's GL extension point.

## Tests and device check

`tests/run.sh` builds a test OSTree ref, verifies the ref branch, metadata,
payload paths and pack contents. The top-level `tests/run.sh` runs this test
alongside the pack and compatibility-proxy suites. `V3Launcher` checks the
runtime's GL extension version list before adding the payload pack mount.

Assigned-VM acceptance procedure (ADB `127.0.0.1:5556`; do not reboot):

1. Install the signed `MatonLinuxSubsystem.apk` produced by `build.sh`.
2. Force-stop `flatpak.app.org_freedesktop_Platform_VulkanInfo.x86_64.v26_08.x39bef4a9006b`.
3. Relaunch it with `monkey -p flatpak.app.org_freedesktop_Platform_VulkanInfo.x86_64.v26_08.x39bef4a9006b 1`.
4. Check `logcat -s MatonV3` for VulkanInfo discovering the Maton ICD and
   listing the forwarder device. Check that EGL's vendor list includes Maton.
   If Vulkan reaches the driver but fails later, record that next error.

The signed subsystem APK installed and relaunched on VM 5556. The runtime
metadata branch check passed, and MatonV3 logged the new GL/maton pack mount.
VulkanInfo no longer reports the old `/run/maton/vulkan/maton_icd.json`
lookup; its next error is `vkCreateInstance: Found no drivers!`. The forwarder
device was therefore not listed. EGL vendor discovery was not independently
verified because the assigned VulkanInfo app does not probe EGL.
