# Embedded PipeWire NDK build

`build-android.sh` builds unmodified PipeWire 1.6.9 with pinned bionic-fill
for libc/header gaps. It adds the fetched directory to the include path and
force-includes `bionic-fill.h` before `compat/android.h`. The former local
`libintl.h` and affinity stub are gone: diagnostic gettext now uses the shared
untranslated fallback, and pthread affinity forwards to sched affinity.

`compat/android.h` retains only the APK-entry `stat()` adapter and unsupported
`pthread_cancel` behavior. Shutdown uses the loop stop callback rather than
cancellation. These are integration choices, not libc fills.

Both the PipeWire and bionic-fill commits are recorded in the build's `SOURCE`.
Set `MATON_BIONIC_FILL_REPO` to a local Git repository to consume an unpublished
pin; otherwise sources come from MatonOS-dev/bionic-fill on GitHub. A cached
revision mismatch fails explicitly; remove only that build's bionic-fill source
directory before changing pins.

Focused NDK 30/API 35 compilation of `thread.c` and `pipewire.c` passed with
`-Werror`; see `../../bfill3.report.md` for commands and limitations. Full builds
and runtime audio validation remain the coordinator's job.
