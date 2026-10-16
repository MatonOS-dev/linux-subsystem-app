# v3.5 package-owned launch

The stub supplies pack mounts from its own installed APK and signer-checked
runtime APKs. PackageDeployment rebuilds a disposable Flatpak lookup tree under
its own cache from each signed index split's deployment.bin. It restores the
original commit object, metadata, deploy GVariant, active/current links and a
files link to the corresponding virtual pack mount. FLATPAK_USER_DIR names this
private tree; FLATPAK_SYSTEM_DIR names its system child. No shared installation,
registered UID pointer or deployment lease is required.

PackageManager owns app registration, content updates and uninstall. Runtime
packages supply runtime/extension payloads; a missing runtime goes through the
existing authenticated store repair API. Extra-data executes at the owning
package UID and is delivered as a content split. Closing the final window keeps
the existing per-process runtime shutdown behavior.

See [the retirement inventory and device tests](../../docs/RETIRE-LINUXD.md).
The host stock-Flatpak test verifies lookup metadata; actual launch, portals,
graphics, runtime repair and lifecycle still require a fresh coordinator image.
