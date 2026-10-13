# Flatpak APEX

`com.matonos.flatpak` packages the bionic Flatpak/OSTree/bubblewrap multicall
binary, stock portal, management wrapper, stage-only store helper, unprivileged
app-flatpak/app-bwrap applet shim and pinned Flathub trust/configuration files.
`prebuilt/static` is the retained artifact directory name, not a musl build.
The multicall and portal are produced by MatonOS_apexs; current pins and original
provenance are in prebuilt/static/SOURCE. Runtime versions use stock Flatpak's
two-root convention; see [storage](../../install/linuxd/STORAGE-LAYOUT.md).

V3Launcher uses matonos-app-flatpak through the compat-proxy loader, at the
stub UID. The shim pins matonos-app-bwrap for nested applet exec and never
changes identity. It is live v3 code. flatpak-env-wrapper accepts management
operations only and rejects run. matonos-flatpak-store performs stage/selftest
only. Both management helpers and the applet shim build in-tree through
tools/build-flatpak-helpers.sh, with fingerprints in HELPERS.sha256.

The v2 bwrap/app-exec helpers, socket/controller wrappers and SysV preload
libraries are deleted from source, artifacts and APEX inventory. There is no
legacy launcher or new SELinux exception. The existing multicall/portal payload
was not rebuilt in this round; integrate the rebuilt management helpers and
updated JNI host/bridge/linuxd together in the coordinator's fresh image.
