# bfill3 report

Commits, not pushed:

- bionic-fill main: `87643d36a708287bb745cfe6482a65c475352db3` — guarded values.h/MAXINT, README first user.
- subsystem wip/bfill3: `2148a4a0b2431c985048536c7f8a9307a874eaae` — migration and docs.
- Report committed separately immediately afterward. All commits have the requested trailers.

## What moved

Both recipes fetch a full pinned Git commit, extract its archive, and check a
REVISION marker. PipeWire records the pin in SOURCE; Xwayland records it in
xwayland-SOURCE. Both accept MATON_BIONIC_FILL_REPO, defaulting to
https://github.com/MatonOS-dev/bionic-fill.git. Until publication, coordinator
must set MATON_BIONIC_FILL_REPO=/home/hanro50/matonos/repos/bionic-fill.
Both actual fetch blocks passed cold local-repository extraction/checks.

PipeWire force-includes bionic-fill.h before compat/android.h. Its libintl.h
and ENOTSUP affinity stub are removed: shared gettext fallback remains
untranslated, while affinity now uses pthread-to-TID sched affinity.
APK stat() and pthread_cancel remain because they implement loader/shutdown
behavior, not libc gaps. Header comment and new PipeWire README explain this.

The Xwayland stack receives the fill include path in autotools C/C++ and
Meson C/C++. Removed generated values.h; recipe cleans up its stale prefix
copy. Cached autotools configurations refresh when the pin changes.
libxshmfence-1.3.3/src/xshmfence_futex.h is the actual values.h consumer and
uses only MAXINT. Cached stack C/header search found no other values.h includes.
New values.h guards MAXINT, maps it to INT_MAX and prefers native include_next.
Empty libpthread.a stays. Subsystem README documents the change.

## Verification

NDK 30.0.16248370 x86_64 android-35; all four final commands below passed
with -Werror. PipeWire reused existing upstream compile-database flags and
generated configuration, replacing old compatibility paths. thread.c and
pipewire.c are the configured embedded consumers of affinity and libintl.
Both libxshmfence translation units compiled, including the futex helper.

```sh
cd /mnt/data/aosp/out/matonos/pipewire/build && /home/hanro50/Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang -Isrc/pipewire/libpipewire-0.3.so.p -Isrc/pipewire -I../src/src/pipewire -Isrc -I../src/src -I. -I../src -I../src/include -Ispa/include -I../src/spa/include -I../src/spa/include-private -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu11 -O3 -fvisibility=hidden -fno-strict-aliasing -fno-strict-overflow -DSPA_AUDIO_MAX_CHANNELS=128u -Wsign-compare -Wpointer-arith -Wpointer-sign -Werror=format -Wno-error=format-overflow -Wformat-security -Wimplicit-fallthrough -Wmissing-braces -Wtype-limits -Wvariadic-macros -Wno-missing-field-initializers -Wno-unused-parameter -Wno-pedantic -Wdeprecated-declarations -Wunused-result -Werror=return-type -Werror=float-conversion -Werror=constant-conversion -D_GNU_SOURCE -DFASTPATH -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion -Werror=old-style-definition -Werror=strict-prototypes -O2 -march=x86-64-v2 -fPIC -pthread -DOLD_MEDIA_SESSION_WORKAROUND=1 -D_POSIX_C_SOURCE -c ../src/src/pipewire/thread.c -I/home/hanro50/matonos/repos/bionic-fill -include /home/hanro50/matonos/repos/bionic-fill/bionic-fill.h -I/home/hanro50/matonos/build/sub-bfill/linux/pipewire/compat -include /home/hanro50/matonos/build/sub-bfill/linux/pipewire/compat/android.h -Werror -o /home/hanro50/matonos/build/sub-bfill/.bfill3-check/thread.o

cd /home/hanro50/matonos/build/sub-bfill && /home/hanro50/Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang -DHAVE_CONFIG_H -I/mnt/data/aosp/out/matonos/compositor/xwayland-stack/libxshmfence-1.3.3 -I/mnt/data/aosp/out/matonos/compositor/sources/src/libxshmfence-1.3.3/src -I/home/hanro50/matonos/repos/bionic-fill -I/mnt/data/aosp/out/matonos/compositor/xstage/x86_64/include -O2 -march=x86-64-v2 -fPIC -Wall -Wextra -Werror -c /mnt/data/aosp/out/matonos/compositor/sources/src/libxshmfence-1.3.3/src/xshmfence_futex.c -o /home/hanro50/matonos/build/sub-bfill/.bfill3-check/xshmfence_futex.c.o

cd /home/hanro50/matonos/build/sub-bfill && /home/hanro50/Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang -DHAVE_CONFIG_H -I/mnt/data/aosp/out/matonos/compositor/xwayland-stack/libxshmfence-1.3.3 -I/mnt/data/aosp/out/matonos/compositor/sources/src/libxshmfence-1.3.3/src -I/home/hanro50/matonos/repos/bionic-fill -I/mnt/data/aosp/out/matonos/compositor/xstage/x86_64/include -O2 -march=x86-64-v2 -fPIC -Wall -Wextra -Werror -c /mnt/data/aosp/out/matonos/compositor/sources/src/libxshmfence-1.3.3/src/xshmfence_alloc.c -o /home/hanro50/matonos/build/sub-bfill/.bfill3-check/xshmfence_alloc.c.o

cd /mnt/data/aosp/out/matonos/pipewire/build && /home/hanro50/Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang -Isrc/pipewire/libpipewire-0.3.so.p -Isrc/pipewire -I../src/src/pipewire -Isrc -I../src/src -I. -I../src -I../src/include -Ispa/include -I../src/spa/include -I../src/spa/include-private -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu11 -O3 -fvisibility=hidden -fno-strict-aliasing -fno-strict-overflow -DSPA_AUDIO_MAX_CHANNELS=128u -Wsign-compare -Wpointer-arith -Wpointer-sign -Werror=format -Wno-error=format-overflow -Wformat-security -Wimplicit-fallthrough -Wmissing-braces -Wtype-limits -Wvariadic-macros -Wno-missing-field-initializers -Wno-unused-parameter -Wno-pedantic -Wdeprecated-declarations -Wunused-result -Werror=return-type -Werror=float-conversion -Werror=constant-conversion -D_GNU_SOURCE -DFASTPATH -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion -Werror=old-style-definition -Werror=strict-prototypes -O2 -march=x86-64-v2 -fPIC -pthread -DOLD_MEDIA_SESSION_WORKAROUND=1 -D_POSIX_C_SOURCE -c ../src/src/pipewire/pipewire.c -I/home/hanro50/matonos/repos/bionic-fill -include /home/hanro50/matonos/repos/bionic-fill/bionic-fill.h -I/home/hanro50/matonos/build/sub-bfill/linux/pipewire/compat -include /home/hanro50/matonos/build/sub-bfill/linux/pipewire/compat/android.h -Werror -o /home/hanro50/matonos/build/sub-bfill/.bfill3-check/pipewire.c.o
```

llvm-nm thread.o shows external sched_setaffinity and pthread_gettid_np:
the old affinity stub is gone. NDK syntax probes with -Wall -Wextra -Werror
included values.h twice and asserted MAXINT == INT_MAX, both with and without
predefined MAXINT; passed. bash -n on both recipes and git diff --check passed.

Exploratory force-inclusion of bionic-fill.h in the POSIX Xwayland stack
failed on GNU-gated cpu_set_t/sched affinity/funopen64 declarations. Final
Xwayland flags use only -I as permitted; PipeWire's -D_GNU_SOURCE allows
force-inclusion there. The exploratory libxshmfence command also omitted its
X11 include prefix; final commands include it. No warning suppression added.

## Other shim inventory

Inspected linux shell build recipes for generated headers, compatibility
flags, missing-symbol edits and sed transformations. No additional clear
header-only libc gap with a demonstrated consumer failure was moved.

| Location | Shim or adaptation | Recommendation |
|---|---|---|
| subsystem/build-compositor.sh, native/compat_shm.[ch] | shm_open/shm_unlink defines, forced prototypes and libmaton-shm | Missing libc API candidate, but current process-local memfd name registry lacks cross-process POSIX semantics. Keep local pending generic semantics/tests. |
| subsystem/build-compositor.sh | empty librt.a | Linker packaging, not header-only fill; keep, like libpthread.a. |
| subsystem/build-xwayland.sh | libgpg-error lock-obj-pub ABI header | Target pthread layout generation; keep upstream generator. |
| dbus-broker/build-glib-{android,ndk}.sh | -D_GNU_SOURCE and compiled proxy-libintl/libintl.a | Feature selection/upstream dependency; keep. Replacing linked intl needs separate configure/link analysis. |
| flatpak/build-support/baseline-compiler.h, build.sh | BoringSSL __has_attribute override | Compiler/CPU selection, not libc; keep scoped to BoringSSL. |
| flatpak/build-support/build.sh | e2fsprogs/gpgme-lite pkg-config declarations; libfyaml 'none required' removal and appstream Requires.private removal | Dependency packaging, not libc; keep outside fills. |
| flatpak/build-support/build-bwrap-musl.sh | libcap static dependency sed edit | musl linking policy; keep outside bionic-fill. |
| subsystem/build-xwayland.sh | GLX/EGL, Popen IDs, SysV SHM and AHB patches | Integration/behavior, not missing libc symbols; outside this migration. |

## Unverified and follow-up

No full builds, links, runtime tests, Soong, Gradle or VMs, per task.
Compilation reused cached pinned upstream sources/generated config; fresh
configure and full-stack build remain coordinator work. Build both recipes
using the local repository override before publication, then test audio and
Xwayland startup. The new bionic-fill commit needs coordinator publication
before default remote fetches work. No shared device-tree/kernel changes.
Scratch is removed after recording commands; recreate output directory when
replaying object commands. No pushes performed.
