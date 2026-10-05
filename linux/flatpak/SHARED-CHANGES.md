# Static Flatpak APEX handoff

`flatpak.mk` includes the one updatable `com.matonos.flatpak` APEX. Its static
multicall ELF supplies Flatpak, OSTree and bubblewrap. The bionic
`flatpak-env-wrapper` and the stage-only `matonos-flatpak-store` remain outside
namespace. The compositor owns portals and the D-Bus session bus; APEX portal,
proxy and GnuPG executables are removed.

The launcher supplies the minimal namespace files and CA path required by the
static stack. `matonos-bwrap` binds those files and the Conscrypt certificate
directory into the app root, along with `/var/tmp -> /tmp`. It does not mount
host `/etc`, `/usr` or `/lib` wholesale.

Static artifact hashes and build provenance live in `prebuilt/static/SOURCE`.
The current Alpine 3.24 artifact is version-correct but still has the old
GPGME/GnuPG backend; replace it with the DullPGP MatonOS_apexs output before
runtime validation. Do not restore bionic shared libraries, a standalone gpg,
xdg-dbus-proxy, native portal, revokefs-fuse or Flatpak triggers to this APEX.
