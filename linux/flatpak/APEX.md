# com.matonos.flatpak APEX (r24)

MatonOS's Flatpak stack ships as one updatable APEX, `com.matonos.flatpak`.
It is preinstalled on the system_ext partition at
`/system_ext/apex/com.matonos.flatpak.apex` and mounted (read-only) at
`/apex/com.matonos.flatpak`. Only linuxd stays in system_ext; it execs the
fixed `/apex/com.matonos.flatpak/bin/flatpak` launcher. The D-Bus broker is
*not* here (it lives in `MatonWaylandHost.apk`).

Everything that Flatpak runs from is in the APEX:

| Path in the APEX | Contents |
|---|---|
| `bin/matonos-flatpak` | Flatpak CLI (seccomp enabled) |
| `bin/flatpak` | NDK launcher (`flatpak-env-wrapper.c`) that linuxd execs |
| `bin/flatpak-portal` | session portal |
| `bin/gpg` | GnuPG for GPGME |
| `bin/ostree`, `bin/revokefs-fuse`, `bin/xdg-dbus-proxy` | helpers |
| `bin/bwrap` | bubblewrap (Soong) |
| `bin/matonos-bwrap` | X11-binding bwrap shim (Soong) |
| `bin/matonos-app-exec` | payload dyntransition launcher (Soong) |
| `bin/matonos-mount-helper` | outside-the-sandbox mount helper (Soong) |
| `bin/matonos-flatpak-store` | store installer helper, `FlatpakStore.c` (Soong) |
| `lib64/*.so` | Flatpak/GPG/GLib/ostree/AppStream dependency set + `libseccomp_matonos.so` |
| `usr/share/flatpak/triggers/*` | Flatpak runtime triggers |

The binaries/libraries built out-of-tree under
`linux/flatpak/prebuilt/system_ext` are wrapped as `cc_prebuilt_binary` /
`cc_prebuilt_library_shared`; the Soong-built ones are ordinary `cc_binary` /
`cc_library_shared` with `apex_available: ["com.matonos.flatpak"]`. All of this
is defined in `linux/flatpak/Android.bp` (and the `bwrap` /
`libseccomp_matonos` modules in `linux/third_party/Android.bp`).

## Data dir and triggers

Flatpak's data files are shipped at the APEX-standard `usr/share` (Soong only
has a `prebuilt_usr_share` module type), i.e.
`/apex/com.matonos.flatpak/usr/share/flatpak/triggers`. The CLI/portal are
built with `--datadir=usr/share`, so their compiled `FLATPAK_DATADIR` matches.
The launcher also exports `FLATPAK_TRIGGERSDIR` to that path, which makes the
current prebuilt binaries (built with the default `share` datadir) look in the
right place and keeps trigger execution correct across a later rebuild.

## Linker namespace

No `LD_LIBRARY_PATH` is used. linkerconfig inspects
`/apex/apex-info-list.xml` and generates a `com.matonos.flatpak` linker
namespace whose search path is the APEX's own `lib64` (and `lib`), falling
back to the system/system_ext namespaces for platform libraries (`libcap.so`,
`libcurl.so`, `libxml2.so`, `libz.so`, `libc++`, …). Binaries executed from
`/apex/com.matonos.flatpak/bin` therefore resolve both APEX-private and
platform libraries automatically. There is no `ld.config.txt` entry to add and
nothing to install in `/system/etc/ld.config.*`. The prebuilt ELFs keep their
harmless build-time `RUNPATH`; it is never needed at runtime.

## SELinux

The APEX carries its own `apex_file_contexts`. It applies the *same* exec types
those binaries had in system_ext (`matonos_bwrap_exec`,
`matonos_app_exec_exec`, `matonos_mount_helper_exec`,
`matonos_flatpak_cli_exec`, `matonos_flatpak_installer_exec`); everything else
stays `system_file`. No new SELinux types, domains, allows or binder
neverallows are added by the move. The corresponding `/system_ext/...` lines
were removed from
`systembridge/sepolicy/system_ext/private/file_contexts`.

Validate it with the same rig used for the platform policy and with `checkfc`:

```sh
# Platform policy rig (system_ext + vendor), pointed at this worktree.
cp -a /mnt/data/aosp/out/pc-logs/agents/codex-app-owns/policy /tmp/opencode/apex-policy
# edit /tmp/opencode/apex-policy/compile.py: work = root/'out/worktrees/flatpak-apex'
python3 /tmp/opencode/apex-policy/compile.py        # must print PASS
out/host/linux-x86/bin/checkfc \
  /tmp/opencode/apex-policy/precompiled_sepolicy \
  linux/flatpak/apex_file_contexts
```

`checkfc` resolves the apex-relative paths as absolute source paths, which is
enough to prove every exec type exists in the compiled policy.

## Versioning

`apex_manifest.json`'s `version` encodes the Flatpak + bubblewrap pair, and is
bumped whenever *either* pin changes. The formula used here is

```
version = flatpak_binary_age * 1000 + bwrap_major * 100 + bwrap_minor
flatpak_binary_age = 10000*major + 100*minor + patch      # Flatpak's own scheme
```

Current pair: Flatpak 1.18.4 + bubblewrap 0.13.0
(`linux/third_party/source-pins.json`) → `11804 * 1000 + 13 = 11804013`.

**Flatpak and bwrap are tested as a pair.** bubblewrap's option table is
mirrored by `matonos-bwrap.c`'s `option_values()`; a stale table corrupts the
launch. Bump both together and regenerate the shim table in the same commit
(see the r24 "Flatpak ↔ bwrap" note in the design docs). Never mix a new
Flatpak CLI with an old bwrap APEX.

## Keys and release TODO

`apex_key com.matonos.flatpak.key` points at the repository dev key
`com.matonos.flatpak.pem` / `com.matonos.flatpak.avbpubkey` (RSA-4096,
`SHA256_RSA4096`, generated with `avbtool extract_public_key`). This key is for
bring-up and `adb install` only.

**Release TODO:** replace both files with the MatonOS release APEX key before
cutting a release. An APEX can only be updated by an APEX signed with the same
key, so a device that shipped the dev key cannot accept a release-key update
(and vice versa); the release image and all later updates must use the release
key. Keep the release private key out of the repository.

## Rebuilding the APEX

The binaries are a tested pair, so a Flatpak or bwrap bump means rebuilding
everything below. On a host with `MATON_AOSP` and zram set up:

```sh
# 1. (only if the pins changed) rebuild the NDK dependency prefix + Flatpak CLI.
#    The APEX-prefix build scripts stage prebuilt/system_ext/bin/*:
linux/flatpak/build-seccomp-apex.sh     # CLI + portal + revokefs, seccomp evidence
linux/flatpak/build-dbus-proxy-apex.sh  # xdg-dbus-proxy
#    (build-seccomp.sh / build-dbus-proxy.sh are the shared base pipeline; the
#    -apex variants add the APEX prefix, a clean $ORIGIN/../lib64 RUNPATH and
#    --datadir=usr/share so the CLI matches the APEX layout.)
#    The GLib/GPGME/ostree/AppStream prefix under out/matonos/flatpak-ndk is
#    built by the same out-of-tree recipe when its pin changes; it is not a
#    Soong module. The Flatpak build uses --prefix=/apex/com.matonos.flatpak
#    and rebuilds the libraries the CLI links.

# 2. rebuild the NDK launcher (uses the APEX paths from this worktree):
tools/build-native.sh                    # or the single clang line it runs

# 3. build the APEX itself (Soong; needs no image):
m com.matonos.flatpak
#    -> out/target/product/pc_x86_64/system_ext/apex/com.matonos.flatpak.apex
#    It is signed with com.matonos.flatpak.key and preinstalled in system_ext.

# 4. (optional) sign an APEX built for a release key:
#    system/apex/apexer/apexer.py already signed it during the build using the
#    apex_key; to re-sign outside Soong use sign_apex.py / apksigner with the
#    same key. Never re-sign with a different key on a shipped device.
```

`flatpak.mk` keeps the seccomp gate: the build fails if
`linux/flatpak/seccomp-config.h` does not say `ENABLE_SECCOMP 1` or if
`bin/matonos-flatpak` does not match `seccomp.sha256`. `build-seccomp.sh`
regenerates both. Keep that invariant when changing the CLI.

## Installing an update on a device

```sh
adb install -r /path/to/com.matonos.flatpak.apex   # userdebug/eng live image
adb reboot
# after boot:
adb shell ls -lZ /apex/com.matonos.flatpak/bin
adb shell su 1000 /apex/com.matonos.flatpak/bin/flatpak --version
```

Installing a *newer* APEX over the preinstalled one only succeeds when it is
signed with the same key as the active version. `apexd` rejects a downgrade
(lower `version`) unless the developer option allows it. After a reboot the
old payload is gone; Flatpak and bwrap update together, as tested.

## AOSP prerequisite: libcap

`bwrap` links `libcap` (`cap_from_name`). `libcap` is an AOSP `external/`
module and its `apex_available` list does not include `com.matonos.flatpak`,
so Soong rejects the APEX analysis with
`"bwrap" requires "libcap" that doesn't list the APEX under 'apex_available'`.

The one-line fix is outside this device tree (the coordinator must carry it
in the AOSP checkout, or as a reviewed fork/patch):

```
external/libcap/Android.bp:
    apex_available: [
        "//apex_available:platform",
        ...
        "com.matonos.flatpak",
    ],
```

`libcap.so` is already installed in `/system/lib64`; the APEX namespace
resolves it there, so it is *not* bundled into the APEX. `target.apex.
exclude_shared_libs` is not used because it would drop the dependency from the
link entirely, leaving `cap_from_name` undefined.