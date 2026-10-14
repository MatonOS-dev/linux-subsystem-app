# i386 APK image extraction (outside this worktree task's edit scope)

`linux/compositor/host/app/build.gradle` packages both executable loaders under
`lib/x86_64/` and preserves their ELF contents. Android extracts APK updates
with the existing extractNativeLibs treatment; the ELF32 loader is deliberately
beside its ELF64 peer, rather than in a separate Android app ABI.

The coordinator must replace the existing three-line single-loader extraction
inside the `matonos-wayland-host` block of `tools/build-apps.sh` with:

```bash
bash "$DEVICE_DIR/linux/compat-proxy/extract-loaders.sh" \
  "$apk" "$DEVICE_DIR/buildinfra/apps-built"
```

That block already runs after signature verification. The helper atomically
extracts both files, rejects missing/empty entries and applies 0755. It does
not change the signing or APK verification path. No edit outside
linux/compat-proxy and linux/compositor/host was made by this agent.

The image currently copies just the ELF64 file in `buildinfra/buildinfra.mk`.
Append its peer to the same PRODUCT_COPY_FILES block:

```make
    device/maton/pc_x86_64/buildinfra/apps-built/libmatonos-v3-loader-i386.so:$(TARGET_COPY_OUT_SYSTEM_EXT)/priv-app/MatonWaylandHost/lib/x86_64/libmatonos-v3-loader-i386.so
```

Continue the preceding ELF64 line with `\` as usual. This preserves the
existing signed APK/nativeLibraryDir trust path. Both shared files are outside
the authorized edit scope. No image build was requested.
