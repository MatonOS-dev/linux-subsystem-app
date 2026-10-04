# Shared changes for the Flatpak prebuilt handoff

- SUPERSEDED (r24 APEX) — `device.mk` still includes
  `linux/flatpak/flatpak.mk`, but that file no longer copies the Flatpak
  binaries/libraries into `/system_ext`. It now adds
  `PRODUCT_PACKAGES += com.matonos.flatpak`; the whole stack is packaged by
  `linux/flatpak/Android.bp` as one updatable APEX preinstalled at
  `/system_ext/apex/com.matonos.flatpak.apex` and mounted at
  `/apex/com.matonos.flatpak`.
- SUPERSEDED (r24 APEX) — `systembridge/sepolicy/system_ext/private/file_contexts`
  no longer labels any `/system_ext/bin/(flatpak|...)` or `/system_ext/lib64`
  Flatpak path. The same exec types are applied inside the APEX by
  `linux/flatpak/apex_file_contexts`; no SELinux types, domains or binder
  neverallows changed.

`install/linuxd` changes only the launcher path (it still execs the fixed
`/apex/com.matonos.flatpak/bin/flatpak`). The prebuilt `flatpak` launcher is a
small NDK wrapper that sets the APEX `PATH`, `FLATPAK_BWRAP`, `FLATPAK` and the
other subprocess paths, then execs the Flatpak ELF payload in the same APEX.
GPGME finds `/apex/com.matonos.flatpak/bin/gpg`; Flatpak finds
`/apex/com.matonos.flatpak/bin/bwrap` (through the `matonos-bwrap` shim).
`LD_LIBRARY_PATH` is gone: the binaries use the APEX linker namespace.
- SUPERSEDED (r24 APEX) — the Flatpak payload and revokefs helper are now APEX
  bin entries, not `/system_ext/bin`. The launcher still sets
  `FLATPAK_REVOKEFS_FUSE` to the helper's fixed APEX bin path (Flatpak's
  supported runtime override).
