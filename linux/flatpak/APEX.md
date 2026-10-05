# `com.matonos.flatpak` APEX: static Alpine stack

The APEX is installed on `system_ext` and mounted at
`/apex/com.matonos.flatpak`. Phase 1 replaces the old bionic dependency tree
with the Alpine 3.24/musl stack: Flatpak 1.16.6, OSTree 2025.7, bubblewrap
0.12.0, DullPGP and BoringSSL. One static multicall ELF provides the
`flatpak`, `ostree` and `bwrap` applets. Applets are selected through `argv[0]`
because Soong drops prebuilt-binary symlinks from the APEX payload. The D-Bus broker remains in
`MatonWaylandHost.apk`; linuxd remains a bionic system component outside the
musl namespace.

## Integration plan: every current APEX member

| Current member | Phase 1 decision | Reason / destination |
|---|---|---|
| Bionic Flatpak CLI | **Replaced by static binary** | `matonos-flatpak`, linked for musl; callers set `argv[0]` to select the applet. |
| `flatpak-env-wrapper` launcher | **Kept (bionic, outside the sandbox)** | APEX entrypoint at `bin/flatpak-env-wrapper`; prepares Android-side launch state and starts the static CLI. |
| `flatpak-portal` | **Dropped** | Portal implementation moves to the compositor Java service. |
| `gpg` | **Dropped** | DullPGP performs OpenPGP operations in-process; no GnuPG child is shipped. |
| `ostree` | **Replaced by static binary** | linuxd passes `argv[0]="ostree"` for OSTree applet calls. |
| `revokefs-fuse` | **Dropped** | Not part of the tested Alpine static payload or user install flow. |
| `xdg-dbus-proxy` | **Dropped** | The global `--socket=session-bus` override is used; the separate proxy is not shipped. |
| `bwrap` | **Replaced by static binary** | Flatpak invokes the labeled `matonos-bwrap` shim, which execs `matonos-flatpak` with `argv[0]="bwrap"`; no bionic shared libraries. |
| `matonos-bwrap` | **Must become static-NDK** | Runs after entering the musl-only root and cannot depend on bionic. |
| `matonos-app-exec` | **Must become static-NDK** | Runs inside the musl namespace as the payload transition launcher. |
| `matonos-mount-helper` | **Dropped** | It is a no-op/vestigial helper and is removed from the launch chain. |
| `matonos-flatpak-store` | **Kept (bionic, outside the sandbox)** | linuxd's privileged storage helper remains outside the app namespace. |
| APEX `lib64` set | **Dropped** | The Flatpak, OSTree and bwrap payload is static; no APEX private shared libraries remain. |
| Flatpak runtime triggers | **Dropped** | Apps are installed by users; trigger scripts are not shipped or run. |

`linux/flatpak/Android.bp` describes this payload. The static ELF and static-NDK
helpers are staged below `prebuilt/static/<arch>/`; only the static multicall
binary is architecture-specific for the current x86_64 product. The key and
remote definition are ordinary APEX `etc` files. The bionic launcher and store
helper stay built as Android binaries and outside the musl namespace. linuxd
execs `flatpak-env-wrapper` with `argv[0]="flatpak"`; the wrapper preserves
`"flatpak"` or `"ostree"` when execing `bin/matonos-flatpak`. Current linuxd
operations use Flatpak; OSTree is linked into the same binary and can be
selected by callers that need its CLI. Flatpak's configured `system_bubblewrap` is
`bin/matonos-bwrap`; the shim executes the same multicall ELF with
`argv[0]="bwrap"`.

Flatpak 1.16.6 has one self-reexec path for its `build-update-repo` worker:
it resolves `/proc/self/exe` to `matonos-flatpak`, then passes the worker
command in `argv[1]`. The multicall entrypoint recognizes the three explicit
applet selectors and routes this real-filename self-reexec to Flatpak's own
command table. Normal OSTree operations use linked libostree; they do not
exec an `ostree` binary by path. OSTree extension commands may use
`ostree-*` from `PATH`, but the MatonOS install/run flow does not invoke such
extensions.

The two-domain SELinux chain keeps distinct executable labels: Flatpak runs
in `matonos_flatpak_run`; the `matonos-bwrap` path has
`matonos_bwrap_exec` and transitions to `matonos_bwrap`; that shim then
executes the real `matonos-flatpak`, labeled `matonos_flatpak_cli_exec`, and
transitions back to `matonos_flatpak_run`. Applet dispatch uses `argv[0]`,
while SELinux labels the actual executable path, so no alias file or symlink
is needed.

## Static namespace setup

The launcher is the boundary between Android's bionic environment and the
musl root. Before starting Flatpak it prepares a private configuration
directory and exports the `FLATPAK_*` settings used by the static stack:

- `/etc/ssl/certs` resolves to `/apex/com.android.conscrypt/cacerts`;
- `/var/tmp` resolves to `/tmp`;
- `/etc/passwd` and `/etc/group` describe the one unprivileged Flatpak user;
- `/etc/resolv.conf` contains the per-app forwarder address (currently a
  loopback stub until linuxd supplies the forwarder);
- `FLATPAK_DOWNLOAD_TMPDIR=/tmp`, Flatpak system/user/cache directories, and
  the static APEX applet paths are set before exec.

The launcher and bwrap shim provide these paths inside the namespace; they do
not bind the host's general `/etc`, `/usr`, or `/lib`. Keep this setup in C in
`flatpak-env-wrapper.c` and `matonos-bwrap.c`, next to the existing namespace
construction.

## Trust files and versions

The minimum tested APEX files are:

- `bin/matonos-flatpak`, with no applet symlinks;
- `bin/matonos-bwrap` and `bin/matonos-app-exec`, static-NDK launch helpers;
- `etc/flatpak/flathub.gpg` and
  `etc/flatpak/remotes.d/flathub.flatpakrepo`.

The Conscrypt CA directory is supplied by the Android image, not copied into
this APEX. The Flathub key fingerprint is
`6E5C05D979C76DAF93C081354184DD4D907A7CAE`.

The Flatpak/bubblewrap pair is deliberately pinned to Alpine 3.24 stable
Flatpak 1.16.6 and bubblewrap 0.12.0 (user decision, 2026-10-05). OSTree is
2025.7. The manifest version keeps the existing encoding:

```
flatpak_age = 10000*major + 100*minor + patch = 11606
bwrap_code = 100*major + minor = 12
apex_version = flatpak_age*1000 + bwrap_code = 11606012
```

Flatpak and bubblewrap are tested as a pair. Keep the version pin and manifest
version synchronized.

## Prebuilt provenance

`prebuilt/static/README.md` describes how the architecture directory is filled
from the `MatonOS-dev/MatonOS_apexs` output. `prebuilt/static/SOURCE` records
the source commit and SHA-256 for every staged executable. Do not put APEX
private keys in that directory. The development APEX key remains
`com.matonos.flatpak.pem` / `com.matonos.flatpak.avbpubkey`; release builds
must use the matching release key.

## Image validation still required

Host-side `tools/preflight.sh` checks the repository structure, but Phase 1
still needs an image build and runtime validation for:

1. APEX assembly/signing and confirmation that the three applet symlinks and
   both static-NDK helpers land at the expected paths.
2. SELinux file-context compilation for the static `matonos-flatpak`,
   `matonos-bwrap`, and `matonos-app-exec` paths. The existing domain and
   transition rules must be checked against the new binary labels.
3. Launcher namespace setup on-device: Conscrypt CA projection, `/var/tmp`,
   generated passwd/group, and DNS forwarder address.
4. User remote-add, signature verification, fresh Flathub pull, offline
   install, and read-only runtime/app execution with no D-Bus service mounted.
5. End-to-end graphical portal behavior after the compositor Java portal
   implementation replaces the native portal.

No AOSP `system/sepolicy` files are changed in this phase. A real image remains
necessary to validate the new APEX file contexts against the platform policy
and exercise the launcher in Android's namespaces.
