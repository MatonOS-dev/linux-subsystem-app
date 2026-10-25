# ICD duplicate host regression — pass 3

Branch `fd-cleanup`, tested at `25ae7a5` on 2026-10-25.

## Finding

`tests/icd-dup-host.py` passes at `4200f8d`, `f8847cc`, and `3250830`, then
fails at `3534867` and `25ae7a5`. The first failing change is therefore
`3534867` (`Reserve fixed proxy descriptor slots`). This disproves the pass-2
claim that the failure was independent of the fd-cleanup work. The EROFS
mutex and SysV arena commits are not implicated by this bisect.

The failure occurs after the bwrap pivot while opening/enumerating
`/usr/share/vulkan/icd.d`; the probe reports `ENOTDIR`. The exact descriptor
state transition that causes it is still unproven. Pass-3 experiments with
native-close metadata cleanup, stale VFS identity clearing on open, and routing
`fstat`/`newfstatat`/`getdents64` through the VFS path did not fix it and were
reverted. Do not treat those experiments as a solution.

## Verification

- `bash linux/compat-proxy/build.sh`: passes at `25ae7a5`.
- `python3 linux/compat-proxy/tests/icd-dup-host.py`: fails in the bwrap
  pivot case with `opendir /usr/share/vulkan/icd.d: Not a directory`.
- `bash linux/compat-proxy/tests/run.sh`: stops at the same final ICD test.
- `python3 linux/compat-proxy/tests/host-command-pair.py`: passes.
- `bash linux/compat-proxy/tests/placeholder-raw.sh` and
  `python3 linux/compat-proxy/tests/placeholder-crash.py`: pass.
- The captured FUSE ldconfig replay previously passed at this same source
  revision; see the pass-2 log/count under
  `/mnt/data/aosp/out/pc-logs/agents/fd-cleanup-pass2-fuse/` and
  `/mnt/data/aosp/out/pc-logs/agents/fd-cleanup-pass2.count`.

Next investigation should trace each open/close/dup/close-range descriptor
number through bwrap's `--bind-fd` setup and pivot, comparing the first bad
state at `3534867` with `3250830`. In particular, determine whether an fd
number reused after a native-range operation retains EROFS/VFS directory
identity or whether a pivot source fd is replaced or swept.
