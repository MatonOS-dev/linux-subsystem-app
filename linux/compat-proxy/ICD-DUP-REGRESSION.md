# ICD duplicate host regression — fixed in pass 4

Branch `fd-cleanup`, diagnosed on 2026-10-26. The first bad commit is
`3534867` (`Reserve fixed proxy descriptor slots`); the pass-3 bisect remains
valid. The pass-2 claim that the failure was independent was incorrect.

## Exact mechanism

The fixed-reserve commit changes the close filter: closes outside the exact
reserve bypass the dispatcher and run natively, whereas the previous filter
routed ordinary closes through metadata cleanup. After bwrap's virtual pivot
and descriptor cleanup, fd **3** is available to the probe. The inherited
`--ro-bind-fd` sources (3, 4, 5, 6 during setup) are not the failing objects:
binding and resolving the runtime, app and GL extension all succeed. The
causal change is the close fast path, not a diagnostic-fd number collision.

The probe opens each of its three ICD aliases with O_PATH and closes it.
These opens reuse fd 3 and attach indexed EROFS metadata for
`/vulkan/icd.d/maton_icd.json` to that number. Ordinary close now runs
natively, leaving the metadata association behind. `erofs_open()` previously
prepended another association rather than replacing the previous lifetime;
several records for fd 3 therefore accumulated.

The subsequent `opendir("/usr/share/vulkan/icd.d")` resolves through the
runtime symlink to the generated GL merge directory. The raw kernel open
succeeds, returning fd 3 for
`/tmp/compat-proxy-<pid>-<nonce>/vulkan/icd.d`. That native open did not clear
indexed metadata. Its following fstat was intercepted by `erofs_call()` and
returned the old manifest's **regular-file** metadata, without issuing a
kernel fstat of the directory. libc's opendir then reported ENOTDIR. Temporary
instrumentation confirmed the indexed object path at that fstat was
`/vulkan/icd.d/maton_icd.json`.

The failing strace is saved outside the repository at
`/mnt/data/aosp/out/pc-logs/agents/fd-cleanup-pass4.strace` (pid 2554833,
near lines 33067–33070). No bind source was relocated incorrectly, no proc-fd
link changed its target incorrectly, and no mount-table entry was corrupted.
It was stale fd-number-keyed metadata after descriptor reuse.

## Fix

A successful native path open clears the reused number's indexed metadata
before recording its new pathname. An indexed open likewise clears the old
lifetime before installing its new indexed record. `erofs_close()` removes
**all** matching records, including duplicates inherited from older state;
removing only the first record can expose an older stale record, explaining
why the earlier cleanup experiments were insufficient.

The indexed-open branch remains separate from the native-open branch so its
newly installed metadata survives. The exact 18-slot top reserve, native
ordinary closes, close-skip guards and move-on-dup guards are unchanged. No
new fd-access checks, allocation skipping or fd relocation rules are added.

## Verification

- `bash linux/compat-proxy/build.sh`: PASS.
- `bash linux/compat-proxy/tests/run.sh`: PASS end to end, exit 0, with no
  GPU/bwrap skip override. This includes host-command-pair, both placeholder
  guards, x86_64/i386 and cross-exec, Android packaging, indexed EROFS, the
  ICD pivot regression, loader maps and site rewriting.
- Standalone host-command-pair and both placeholder guards: PASS.
- Captured ldconfig replay: PASS, exit 0; cache written. It reused the
  previously extracted capture layers through a scratch symlink, with fresh
  root/table scaffolding and a new execution trace.

Logs and the final report live under `/mnt/data/aosp/out/pc-logs/agents/`,
with prefix `fd-cleanup-pass4` and report `fd-cleanup.final.md`.
No VM, AOSP build, shared-device-tree edit or kernel change is needed.
