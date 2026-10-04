# Static APEX payload staging

Copy release artifacts from the reviewed `MatonOS-dev/MatonOS_apexs` output
into `static/<arch>/`. Record the exact source repository commit and SHA-256
for each executable in `static/SOURCE`. For the current x86_64 image the
required files are `matonos-flatpak`, `matonos-bwrap`, and
`matonos-app-exec`. The latter two are static-NDK launch glue; they must not
link to bionic shared libraries because they execute in the musl namespace.

The multicall binary must provide Flatpak 1.16.6, OSTree 2025.7 and
bubblewrap 0.12.0 applets and use the DullPGP implementation linked with
BoringSSL. Do not add a standalone `gpg`, `xdg-dbus-proxy`, portal or runtime
trigger to this APEX.
