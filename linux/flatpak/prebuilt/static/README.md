# Static APEX payload staging

Run `linux/flatpak/stage-static-musl.sh` to refresh checksum-verified helper
inputs, build in the pinned Alpine root, and stage the output from
`MatonOS_apexs` into `static/<arch>/`. The canonical helper sources live in
`linux/flatpak/`; `MatonOS_apexs/flatpak/helpers/` contains generated build
copies. `static/SOURCE` records artifact and source SHA-256 hashes. The three
required files are `matonos-flatpak`, `matonos-bwrap`, and
`matonos-app-exec`; all are stripped static musl PIE files.

The multicall binary must provide Flatpak 1.16.6, OSTree 2025.7 and
bubblewrap 0.12.0 applets and use the DullPGP implementation linked with
BoringSSL. Do not add a standalone `gpg`, `xdg-dbus-proxy`, portal or runtime
trigger to this APEX.
