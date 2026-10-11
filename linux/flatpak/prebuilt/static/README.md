# Static APEX payload staging

Helpers are bionic (NDK r30, API 36); the Alpine/musl recipe is retired.
`tools/build-flatpak-helpers.sh` (run by build.sh) builds `flatpak-env-wrapper`,
`matonos-bwrap`, `matonos-app-tool` and the SysV shm preload libraries in-tree.
`linux/flatpak/stage-apex-helpers.sh` syncs helper sources into
`MatonOS_apexs/flatpak/helpers/` (generated copies; do not edit them) and
installs `matonos-app-exec` and `matonos-flatpak-store` built there. The
Flatpak multicall, portal and `static/SOURCE` come from MatonOS_apexs
`flatpak/build.sh`. `matonos-flatpak-store` supports only signed-ref staging
and `selftest`.

The multicall binary must provide Flatpak 1.16.6, OSTree 2025.7 and
bubblewrap 0.12.0 applets and use the DullPGP implementation linked with
BoringSSL. Do not add a standalone `gpg`, `xdg-dbus-proxy`, portal or runtime
trigger to this APEX.
