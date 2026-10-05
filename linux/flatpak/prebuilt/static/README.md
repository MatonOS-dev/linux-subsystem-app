# Static APEX payload staging

Run `linux/flatpak/stage-static-musl.sh` to refresh checksum-verified helper
inputs, build in the pinned Alpine root, and stage the output from
`MatonOS_apexs` into `static/<arch>/`. The canonical helper sources live in
`linux/flatpak/`; `MatonOS_apexs/flatpak/helpers/` contains generated build
copies. `static/SOURCE` records artifact and source SHA-256 hashes. The five
APEX binaries are `matonos-flatpak`, `matonos-bwrap`, `matonos-app-exec`,
`flatpak-env-wrapper`, and `matonos-flatpak-store`; all are stripped static
musl PIE files. `matonos-flatpak-store` supports only signed-ref staging and
`selftest`. The host-only launch probes are built in the Alpine output
directory and are not staged into the APEX.

The multicall binary must provide Flatpak 1.16.6, OSTree 2025.7 and
bubblewrap 0.12.0 applets and use the DullPGP implementation linked with
BoringSSL. Do not add a standalone `gpg`, `xdg-dbus-proxy`, portal or runtime
trigger to this APEX.
