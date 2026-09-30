# Shared changes for the Flatpak prebuilt handoff

- DONE — `device.mk`: added a `# flatpak-spike:` product include for
  `linux/flatpak/flatpak.mk`, so the fixed prebuilts enter system_ext.
- DONE — `systembridge/sepolicy/system_ext/private/file_contexts`: added
  `# flatpak-spike:` file-context entries for the CLI launcher, runtime
  binaries/libraries, Flatpak helpers, and trigger data. They use the existing
  `system_file` type already allowed by `matonos-linuxd`; no policy files or
  SELinux types were added.

The install/linuxd sources were not changed. The `/system_ext/bin/flatpak`
prebuilt is a small NDK launcher that sets the image PATH, library path, and
`FLATPAK_BWRAP`, then execs the Flatpak ELF payload. This lets GPGME find
`/system_ext/bin/gpg` and Flatpak find the existing `/system_ext/bin/bwrap`.
- UPDATED — Flatpak payload and revokefs helper are installed under
  `/system_ext/bin`. Soong fsgen classifies destinations by string prefix, so the `libexec`
  target was mistaken for the `lib` prebuilt path and rejected the first attempt (`Path is outside
  directory: ../libexec`). The launcher sets `FLATPAK_REVOKEFS_FUSE` to the
  helper's fixed bin path; this uses Flatpak's supported runtime override.
